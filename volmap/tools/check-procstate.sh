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
# What volmap says about the processes beside a database comes from /proc alone, and
# it must say the right thing:
#   - a running server is "running" with its pid, registered with cub_master;
#   - a database with no server is "not_running";
#   - a backupdb on the database is reported while it runs (batch head, JSON
#     maintenance, and the interactive bar with the scan paused) and not after;
#   - a TDE note never makes --check exit 2 on its own (checked on the JSON shape).
# Needs: a running database RUNDB (default demodb) that csql's dba can back up without a
# password: backupdb -C writes the backup into a temporary directory that is deleted
# afterwards - note that it also records that backup in the database's _bkvinf file and
# in the log header (a stale entry a later restoredb will not find).  STOPDB (a name or
# a vinf path) is a database with no server; OLDDB a running server before 11.1.  When
# RUNDB is not running the test is skipped.  Signals from /proc are informational and
# unauthenticated: any local user could fake them, so this checks the reading, not a
# security property.
#
#   RUNDB=demodb STOPDB=/path/to/x_vinf OLDDB=/path/to/old_vinf sh tools/check-procstate.sh
#   (OLDDB: a running server of a release before 11.1, optional)
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
BIN=${VOLMAP:-$ROOT/cub_volmap}
RUNDB=${RUNDB:-demodb}
TMP=$(mktemp -d /tmp/volmap-proc.XXXXXX)
rc=0
# the backup runs in its own process group: cubrid forks cub_admin, so killing the
# wrapper alone would leave the real backup running
cleanup(){ [ -n "$bp" ] && kill -- -"$bp" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
[ -x "$BIN" ] || { echo "→ FAIL (no binary: $BIN)"; exit 1; }
command -v cubrid >/dev/null || { echo "→ SKIP (no cubrid in PATH)"; exit 0; }
j(){ python3 -c "import sys,json; d=json.load(sys.stdin); print($1)"; }
if [ "$("$BIN" --format=json "$RUNDB" 2>/dev/null | j "d['server']['state']")" != running ]; then
  echo "→ SKIP ($RUNDB has no running server on this host)"; exit 0; fi
# 1. the running server
st=$("$BIN" --format=json "$RUNDB" 2>/dev/null | j "d['server']['state'], d['server']['registered'], d['server']['pid']>0")
if [ "$st" = "running True True" ]; then echo "  ok     $RUNDB: server running, registered with cub_master (from /proc/net/unix)"
else echo "  FAIL   $RUNDB: server state '$st' (want running True True)"; rc=1; fi
# 2. a database without a server
if [ -n "$STOPDB" ]; then
  st=$("$BIN" --format=json "$STOPDB" 2>/dev/null | j "d['server']['state'], d['maintenance']")
  if [ "$st" = "not_running None" ]; then echo "  ok     $STOPDB: no server, no utility"
  else echo "  FAIL   $STOPDB: '$st' (want not_running None)"; rc=1; fi
fi
# 2b. a running server of a release before 11.1 (OLDDB: name or vinf path): its registration
#     socket has no pid in its name, so it must be "running_unverified", hold the active log
#     lock, and not hold the interactive scan
if [ -n "$OLDDB" ]; then
  st=$("$BIN" --format=json "$OLDDB" 2>/dev/null | j "d['server']['state'], d['server']['registered'], d['server']['log_lock_pid']==d['server']['pid'], d['release']")
  case "$st" in
    "running_unverified None True "*) echo "  ok     $OLDDB: release ${st##* } server running_unverified, registration null, holds the log lock" ;;
    *) echo "  FAIL   $OLDDB: '$st' (want running_unverified None True <release>)"; rc=1 ;;
  esac
  cat > "$TMP/old.py" <<'PY'
import os, sys, pty, time, select, fcntl, termios, struct, signal, re
binp, db = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"; os.execv(binp, [binp, "-i", db])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 50, 200, 0, 0))
buf = b""; t0 = time.time()
while time.time() - t0 < 3:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try: buf += os.read(fd, 65536)
        except OSError: break
txt = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", buf).decode("utf-8", "replace")
print("held" if "paused" in txt else "not-held")
os.write(fd, b"q")
for _ in range(50):
    try:
        if os.waitpid(pid, os.WNOHANG)[0] == pid: break
    except ChildProcessError: break
    time.sleep(0.1)
else:
    os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0)
PY
  held=$(timeout 20 python3 "$TMP/old.py" "$BIN" "$OLDDB" 2>/dev/null)
  if [ "$held" = not-held ]; then echo "  ok     $OLDDB: -i starts without a hold"
  else echo "  FAIL   $OLDDB: -i hold = $held"; rc=1; fi
fi
# 3. backupdb while it runs: batch head, JSON, interactive bar; and gone afterwards
mkdir -p "$TMP/bk"
setsid sh -c 'exec cubrid backupdb -C -D "$0" "$1"' "$TMP/bk" "$RUNDB" > "$TMP/bk.out" 2>&1 & bp=$!
sleep 1.5
alive(){ kill -0 "$bp" 2>/dev/null; }
alive || { echo "  SKIP   backupdb of $RUNDB finished in under 1.5s: too short to observe (use a larger RUNDB)"; bp=""; }
head=$(alive && "$BIN" --plain "$RUNDB" 2>/dev/null | grep -c 'maintenance: cubrid backupdb is running')
mt=$(alive && "$BIN" --format=json "$RUNDB" 2>/dev/null | j "d['maintenance']['utility'] if d['maintenance'] else None")
cat > "$TMP/bar.py" <<'PY'
import os, sys, pty, time, select, fcntl, termios, struct, signal, re
binp, db = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"; os.execv(binp, [binp, "-i", db])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 50, 200, 0, 0))
buf = b""; t0 = time.time()
while time.time() - t0 < 3:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try: buf += os.read(fd, 65536)
        except OSError: break
txt = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", buf).decode("utf-8", "replace")
print("paused" if "backupdb running" in txt and "paused" in txt else "no-bar")
os.write(fd, b"q")
for _ in range(50):                       # bounded: a pty child that ignores q is killed
    try:
        if os.waitpid(pid, os.WNOHANG)[0] == pid: break
    except ChildProcessError: break
    time.sleep(0.1)
else:
    os.kill(pid, signal.SIGKILL); os.waitpid(pid, 0)
PY
bar=$(alive && timeout 20 python3 "$TMP/bar.py" "$BIN" "$RUNDB" 2>/dev/null)
if [ -n "$bp" ]; then wait $bp; bprc=$?; bp=""; else bprc=skipped; fi
after=$("$BIN" --format=json "$RUNDB" 2>/dev/null | j "d['maintenance']")
if [ "$bprc" = skipped ]; then :
elif [ "$head" = 1 ] && [ "$mt" = backupdb ] && [ "$bar" = paused ] && [ "$after" = None ]; then
  echo "  ok     backupdb on $RUNDB seen while it ran (head, JSON, -i bar paused) and not after (backup rc $bprc)"
else echo "  FAIL   backupdb detection: head=$head json=$mt bar=$bar after=$after (backup rc $bprc)"; rc=1; fi
# 4. notes are not findings: a run whose findings[] is empty must not exit 2 whatever
#    notes[] holds (on a TDE database notes[] carries tde_encrypted_pages; elsewhere it is
#    empty and only the exit-code part of the check is meaningful)
"$BIN" --format=json --check "$RUNDB" > "$TMP/chk.json" 2>/dev/null; crc=$?
nt=$(j "len(d['findings']), len(d['notes']), all('finding' not in n for n in d['notes'])" < "$TMP/chk.json")
set -- $nt
if [ "$3" = True ] && { [ "$1" != 0 ] || [ $crc -ne 2 ]; }; then
  echo "  ok     notes[] ($2) are apart from findings[] ($1): exit $crc"
else echo "  FAIL   notes/findings/exit: findings $1 notes $2 keyed-right $3 exit $crc"; rc=1; fi
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
