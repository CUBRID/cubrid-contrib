#!/bin/bash
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
# Regression: a recording must replay to the same values it captured.
#
# The check is mechanical rather than visual: --replay ... -d re-emits the frame
# through the same terse path the recorder used, so a recorded frame and its
# replay must agree key for key.  Values that are true of the run rather than of
# the frame (which options were passed, whether this is a replay) are excluded.
#
# Without this, "replay looks right" would be a judgement; here it is a diff.
#
# Frames with the server down (conf.* from cubrid.conf in place of param.*) are recorded
# inside a new PID namespace, where no cub_server is visible, so that case runs with or
# without a server.  The other checks need a recording: one made now when a server runs,
# else examples/10-recording.jsonl (CI), where only the key-for-key round trips are skipped.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/cub_top"
[ -x "$BIN" ] || { echo "SKIP: no binary ($BIN) - run build.sh first"; exit 0; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
REC="$TMP/rec.jsonl"
chmod 755 "$TMP"
# Recorded frames against their replay, every key both ways
cat > "$TMP/cmp.py" <<'PY'
import json,sys
# Every key both ways.  With "example" as the third argument the recording is
# examples/10-recording.jsonl, this build's reference recording: a key it lacks means this
# build writes a new key, and the example must be re-recorded.
example=len(sys.argv)>3 and sys.argv[3]=="example"
rec=[json.loads(l) for l in open(sys.argv[1]) if l.strip().startswith('{')]
rep=[json.loads(l) for l in open(sys.argv[2]) if l.strip().startswith('{')]
if len(rep)!=len(rec):
    print(f"  FAIL replay emitted {len(rep)} frame(s) for {len(rec)} recorded"); sys.exit(1)
# Every frame, both ways: a key the replay drops, a key it invents (a 0 where the
# recording had no measurement) and a changed value all fail.  Only how the run was
# invoked is a property of the run rather than of the frame.
SKIP={'opts.used'}
diff=[]; n=0
for fi,(a,b) in enumerate(zip(rec,rep)):
    for k in sorted(set(a)|set(b)):
        if k in SKIP: continue
        n+=1
        if k not in b: diff.append((fi,k,a[k],'<missing>')); continue
        if k not in a: diff.append((fi,k,'<absent>',b[k])); continue
        v,w=a[k],b[k]
        if isinstance(v,(int,float)) and isinstance(w,(int,float)):
            if abs(float(v)-float(w))>max(1e-6,abs(float(v))*1e-9): diff.append((fi,k,v,w))
        elif v!=w: diff.append((fi,k,v,w))
if diff:
    print(f"  FAIL {len(diff)} key(s) differ after replay:")
    for fi,k,a,b in diff[:8]: print(f"        frame {fi} {k}: recorded={a} replayed={b}")
    if example and any(a=='<absent>' for _,_,a,_ in diff):
        print("        (keys the example lacks: this build writes new keys - re-record examples/10-recording.jsonl with it)")
    sys.exit(1)
print(f"  ok   replay reproduces every frame ({len(rec)} frames, {n} keys compared both ways, {len(SKIP)} run property skipped)")
PY

bad=0
finish(){ [ $bad -eq 0 ] && { echo "-> PASS"; exit 0; }; echo "-> FAIL"; exit 1; }
# 0) the server down: recorded where no cub_server is visible, replayed key for key
DREC="$TMP/down.jsonl"
if [ "$(id -u)" = 0 ]; then NS="unshare -pf --mount-proc"; else NS="unshare -Urpf --mount-proc"; fi
if $NS true 2>/dev/null && command -v python3 >/dev/null; then
    $NS sh -c "'$BIN' -b --interval 0.5 --record '$DREC' >/dev/null 2>&1 & sleep 2.5; kill -TERM \$!; wait" 2>/dev/null
    if [ -s "$DREC" ] && grep -q '"server.up": 0' "$DREC"; then
        nc=$(head -1 "$DREC" | grep -o '"conf\.' | wc -l)
        "$BIN" --replay "$DREC" --dump-json >"$TMP/down.out" 2>/dev/null
        if python3 "$TMP/cmp.py" "$DREC" "$TMP/down.out" >"$TMP/down.res"; then
            echo "  ok   server down: $(grep -c . "$DREC") frame(s) with $nc conf.* key(s) each replay key for key"
            [ "$nc" -gt 0 ] || echo "  note no conf.* recorded (\$CUBRID/conf/cubrid.conf not readable)"
        else sed 's/^/  /' "$TMP/down.res"; echo "  FAIL server-down frames"; bad=1; fi
    else echo "  FAIL no server-down frame recorded in a PID namespace"; bad=1; fi
else echo "  SKIP server-down frames (no PID namespace: unshare -p not permitted)"; fi

# The rest works on a recording.  With a cub_server running it is recorded now by this build;
# without one (CI) they run on examples/10-recording.jsonl, this build's reference recording
# (3 frames of cbench at --interval 1): the round trips compare it both ways like a fresh
# recording, and only the sub-second gap check (0.5s frames recorded here) is skipped.  A
# build that writes new keys must re-record the example.  Replays run at speed 0: the
# recorded gaps are not waited for.
LIVE=1; MODE=""
if "$BIN" -d 2>/dev/null | grep -q '^server.up=1'; then
    # --record is live-only (-b/-p); a short live run records a few frames without a tty.
    "$BIN" -b --record "$REC" >/dev/null 2>&1 &
    p=$!; sleep 3; kill -TERM $p 2>/dev/null; wait $p 2>/dev/null
    [ -s "$REC" ] || { echo "  FAIL no frames recorded"; echo "-> FAIL"; exit 1; }
else
    LIVE=0; MODE=example
    [ -s "$ROOT/examples/10-recording.jsonl" ] || { echo "  SKIP recorded frames: no cub_server and no examples/10-recording.jsonl"; finish; }
    cp "$ROOT/examples/10-recording.jsonl" "$REC"
    echo "  note cub_server not running: the recording checks use examples/10-recording.jsonl"
    # This build must still accept the example, or every check below fails for the wrong reason
    want=$(grep -c '^{' "$REC"); got=$("$BIN" --replay "$REC" --dump-json --replay-speed 0 2>/dev/null | grep -c '^{')
    [ "$got" = "$want" ] || { echo "  FAIL this build replays $got of the example's $want frame(s): re-record examples/10-recording.jsonl with this build"; bad=1; finish; }
fi

N=$(grep -c '^{' "$REC")
[ $LIVE = 1 ] && echo "recorded $N frame(s)" || echo "example $N frame(s)"

# 1) every line is one self-contained JSON object
if command -v python3 >/dev/null; then
    python3 - "$REC" "$LIVE" <<'PY' || bad=1
import json,sys
n=0
for ln in open(sys.argv[1]):
    ln=ln.strip()
    if not ln: continue
    n+=1
    try: json.loads(ln)
    except Exception as e:
        print(f"  FAIL line {n} is not valid JSON: {e}"); sys.exit(1)
print(f"  ok   {n} line(s) valid JSON")
# Frame times carry the sub-second interval: ts_epoch_ms strictly increasing, and at the
# default 0.5s interval at least one gap below a second (whole seconds would lose it)
ms=[json.loads(l)['ts_epoch_ms'] for l in open(sys.argv[1]) if l.strip()]
if any(b<=a for a,b in zip(ms,ms[1:])):
    print("  FAIL ts_epoch_ms not strictly increasing"); sys.exit(1)
if sys.argv[2]=="1" and len(ms)>2 and not any(b-a<1000 for a,b in zip(ms,ms[1:])):   # recorded here at 0.5s
    print("  FAIL no sub-second frame gap recorded"); sys.exit(1)
print(f"  ok   frame times in ms, gaps {[int(b-a) for a,b in zip(ms,ms[1:])][:6]}")
PY
else
    echo "  skip JSON validation (no python3)"
fi

# 2) round trip: replay re-emits what was recorded.  --dump-json keeps quoted strings whole
#    and numbers typed, so text keys (reasons, verdict) compare exactly.
"$BIN" --replay "$REC" --dump-json --replay-speed 0 >"$TMP/out" 2>/dev/null
if [ ! -s "$TMP/out" ]; then
    echo "  FAIL --replay --dump-json produced nothing"; bad=1
else
    if command -v python3 >/dev/null; then
        python3 "$TMP/cmp.py" "$REC" "$TMP/out" $MODE || bad=1
    else
        echo "  skip round-trip comparison (no python3)"
    fi
fi

# 2a) a frame whose paramdump was skipped (the server is another account's) keeps the reason
if command -v python3 >/dev/null; then
    python3 - "$REC" "$TMP/skip.jsonl" <<'PY'
import json,sys
d=json.loads(open(sys.argv[1]).readline()); d['param.skipped_reason']='서버 설치 위치를 읽을 수 없음(다른 계정) — paramdump 생략'
open(sys.argv[2],'w').write(json.dumps(d,ensure_ascii=False)+'\n')
PY
    "$BIN" --replay "$TMP/skip.jsonl" --dump-json --replay-speed 0 >"$TMP/skip.out" 2>/dev/null
    python3 "$TMP/cmp.py" "$TMP/skip.jsonl" "$TMP/skip.out" $MODE >"$TMP/skip.res" \
        && echo "  ok   param.skipped_reason replays" || { sed 's/^/  /' "$TMP/skip.res"; bad=1; }
fi

# 2c) a cut line (SIGKILL, full disk, power loss) is skipped with a warning, not replayed with
#     its missing keys as 0; and the output does not depend on the viewer's TZ
L1=$(head -1 "$REC")
{ printf '%s\n' "$L1"; printf '%s' "$L1" | head -c 3000; echo; printf '%s' "$L1" | head -c 30; echo; printf '%s\n' "$L1"; printf '%s' "$L1" | head -c 4000; } >"$TMP/cut.jsonl"
"$BIN" --replay "$TMP/cut.jsonl" -d --replay-speed 0 >"$TMP/cut.out" 2>"$TMP/cut.err"
nf=$(grep -c '^ts=' "$TMP/cut.out"); n70=$(grep -c '^ts="1970' "$TMP/cut.out")
if [ "$nf" = 2 ] && [ "$n70" = 0 ] && grep -q ' 3 ' "$TMP/cut.err" && grep -q '2 3 5)' "$TMP/cut.err"; then
    echo "  ok   cut lines skipped: 2 whole frames of 5 lines, warning \"$(tail -1 "$TMP/cut.err" | sed 's/.*: //')\""
else echo "  FAIL cut lines: $nf frame(s) replayed, $n70 at 1970; stderr: $(tail -1 "$TMP/cut.err")"; bad=1; fi
a=$(TZ=UTC "$BIN" --replay "$REC" --dump-json --replay-speed 0 2>/dev/null | md5sum)
b=$(TZ=Asia/Seoul "$BIN" --replay "$REC" --dump-json --replay-speed 0 2>/dev/null | md5sum)
c=$(TZ=America/New_York "$BIN" --replay "$REC" -d --replay-speed 0 2>/dev/null | grep -m1 '^ts=' | sed 's/ ts_epoch=.*//')
want_ts=$(printf '%s' "$L1" | sed -n 's/.*"ts": *\("[^"]*"\).*/ts=\1/p')   # no python3 needed here
if [ "$a" = "$b" ] && [ -n "$want_ts" ] && [ "$c" = "$want_ts" ]; then
    echo "  ok   replay output is the same under UTC, Asia/Seoul and America/New_York ($c)"
else echo "  FAIL replay depends on TZ ($c)"; bad=1; fi

# 2d) options a replay cannot apply are refused with status 2, not silently ignored
nrej=0; for o in "-d nosuchdb" "-d cbench" "--bcb-dump $TMP/x.bin" "-d --interval 2" \
                 "-d --offsets $TMP" "-d --pgbuf-detail" "-d --dump-hist" "-d --record-overwrite"; do
    "$BIN" --replay "$REC" $o </dev/null >/dev/null 2>"$TMP/oerr"; rc=$?
    if [ $rc = 2 ] && grep -q -- '--replay' "$TMP/oerr"; then nrej=$((nrej+1)); else echo "  FAIL --replay with $o: exit $rc"; bad=1; fi
done
[ -e "$TMP/x.bin" ] && { echo "  FAIL --bcb-dump wrote a file during a replay"; bad=1; }
nacc=0; for o in "-d --ascii" "--dump-json --replay-speed 0"; do
    "$BIN" --replay "$REC" $o </dev/null >/dev/null 2>&1 && nacc=$((nacc+1)) || { echo "  FAIL --replay with $o refused"; bad=1; }
done
[ $nrej = 8 ] && [ $nacc = 2 ] && echo "  ok   8 inapplicable option sets refused with status 2, 2 applicable ones accepted"
# An option qualifying an absent one: --replay-speed without --replay, --record-overwrite without --record
nq=0; for o in "-d --replay-speed 2" "-b --replay-speed 0" "-d --record-overwrite" "-b --record-overwrite" "--replay-speed 2" "--record-overwrite"; do
    "$BIN" $o </dev/null >/dev/null 2>"$TMP/qerr"; rc=$?
    if [ $rc = 2 ] && grep -q -- 'with --re\|와 함께만' "$TMP/qerr"; then nq=$((nq+1)); else echo "  FAIL $o: exit $rc ($(head -1 "$TMP/qerr" | cut -c1-60))"; bad=1; fi
done
[ $nq = 6 ] && echo "  ok   --replay-speed without --replay and --record-overwrite without --record: refused with status 2 (6 forms)"

# 2b') a profile cell at a rounding edge: read_ratio is printed to 0.01, so a recorded
#      write-heavy frame at 0.299 reads back as 0.30 - the recorded cell must win
if command -v python3 >/dev/null && grep -q '"cap.profile"' "$REC"; then
    python3 - "$REC" "$TMP/edge.jsonl" <<'PY'
import json,sys
d=json.loads(open(sys.argv[1]).readline())
d['cap.read_ratio']=0.30; d['cap.profile']='>64KB/write-heavy'
open(sys.argv[2],'w').write(json.dumps(d,ensure_ascii=False)+'\n')
PY
    got=$("$BIN" --replay "$TMP/edge.jsonl" --dump-json --replay-speed 0 2>/dev/null | python3 -c 'import json,sys; print(json.loads(sys.stdin.readline()).get("cap.profile"))')
    if [ "$got" = ">64KB/write-heavy" ]; then echo "  ok   profile cell at a rounding edge replays as recorded"
    else echo "  FAIL profile cell at a rounding edge replayed as $got"; bad=1; fi
fi

# 2c) the plot view says it is a replay and when it was recorded (driven in a pty)
if command -v python3 >/dev/null; then
    python3 - "$BIN" "$REC" <<'PY' || bad=1
import os,pty,subprocess,select,time,re,sys,fcntl,termios,struct
binp,rec=sys.argv[1],sys.argv[2]
m,s=pty.openpty(); fcntl.ioctl(s,termios.TIOCSWINSZ,struct.pack('HHHH',60,160,0,0))
pr=subprocess.Popen([binp,'--replay',rec,'-p','--replay-speed','0.2'],stdin=s,stdout=s,
                    stderr=subprocess.DEVNULL,close_fds=True)
os.close(s); out=b''; t=time.time()
while time.time()-t<3 and pr.poll() is None:
    r,_,_=select.select([m],[],[],0.2)
    if r:
        try: out+=os.read(m,1<<16)
        except OSError: break
try: os.write(m,b'q')
except OSError: pass
try: pr.wait(timeout=3)
except Exception: pr.kill()
os.close(m)
txt=re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',out.decode('utf-8','replace'))
mark=re.search(r'(재생|REPLAY) \d+/\d+',txt); when=re.search(r'(기록|recorded) \d{4}-\d\d-\d\d \d\d:\d\d:\d\d',txt)
if not (mark and when):
    print(f"  FAIL plot view lacks the replay mark or recorded time (mark={bool(mark)} time={bool(when)})"); sys.exit(1)
print(f"  ok   plot view shows \"{mark.group(0)}\" and \"{when.group(0)}\"")
PY
fi

# 2d) a command rejected for its arguments must leave an existing recording untouched
cp "$REC" "$TMP/keep.jsonl"
for args in "-t -b" "-d -p" "-b no_such_db_x" "-t"; do
    # shellcheck disable=SC2086
    "$BIN" $args --record "$TMP/keep.jsonl" </dev/null >/dev/null 2>&1
    if ! cmp -s "$REC" "$TMP/keep.jsonl"; then
        echo "  FAIL rejected command ($args --record) emptied the recording"; bad=1
        cp "$REC" "$TMP/keep.jsonl"
    fi
done
[ $bad -eq 0 ] && echo "  ok   rejected commands leave an existing recording intact"

# 3) a replay must read no /proc of any process - it is a file, not a measurement
if command -v strace >/dev/null; then
    strace -f -e trace=openat -o "$TMP/st" "$BIN" --replay "$REC" -d --replay-speed 0 >/dev/null 2>&1
    n=$(grep -cE '"/proc/[0-9]' "$TMP/st" 2>/dev/null | head -1)
    [ -n "$n" ] || n=0
    if [ "${n:-0}" -ne 0 ]; then
        echo "  FAIL replay opened /proc/<pid> $n time(s) - it must draw only from the file"; bad=1
    else
        echo "  ok   replay reads no /proc/<pid>"
    fi
else
    echo "  skip /proc isolation check (no strace)"
fi

# 4) a recording this build did not make (an older build wrote tier memory as *_pss_bytes) is
#    refused with the reason, rather than replayed with master/broker/CAS/PL at 0
sed 's/\(tier\.[a-z]*\)_rss_bytes"/\1_pss_bytes"/g' "$REC" > "$TMP/older.jsonl"
"$BIN" --replay "$TMP/older.jsonl" -d --replay-speed 0 >"$TMP/older.out" 2>"$TMP/older.err"; e=$?
if [ "$e" = 1 ] && [ ! -s "$TMP/older.out" ] && grep -q '_pss_bytes' "$TMP/older.err"; then
    echo "  ok   older build's recording refused: $(head -1 "$TMP/older.err" | sed 's/^cub_top: --replay [^:]*: //' | cut -c1-80)"
else
    echo "  FAIL older build's recording: exit $e, $(grep -c '^ts=' "$TMP/older.out") frame(s) shown"; bad=1
fi

# 8) the DB disk's utilization is its own key: a recording that lacks cap.dev_util (made
#    before it was written) must replay as "DB disk unknown", never with the host's busiest
#    device standing in (hostdev.util_pct) and never as 0%
if command -v python3 >/dev/null; then
    python3 - "$REC" "$TMP/nodev.jsonl" <<'PY'
import re,sys
s=open(sys.argv[1]).read(); open(sys.argv[2],"w").write(re.sub(r'"cap\.dev_util":\s*[0-9.\-]+,\s*','',s))
PY
    d1=$("$BIN" --replay "$REC" -d --replay-speed 0 2>/dev/null | grep -o 'cap.dev_util=[0-9.-]*' | head -1)
    case "$d1" in cap.dev_util=-1*|"") echo "  FAIL replay with cap.dev_util: $d1"; bad=1 ;; *) ;; esac
    d0=$("$BIN" --replay "$TMP/nodev.jsonl" -d --replay-speed 0 2>/dev/null | grep -o 'cap.device="[^"]*"\|cap.device_reason="[^"]*"' | head -2 | tr '\n' ' ')
    case "$d0" in 'cap.device="" cap.device_reason="'*cap.dev_util*) echo "  ok   recording without cap.dev_util replays as DB disk unknown ($d0)" ;;
                  *) echo "  FAIL recording without cap.dev_util: $d0"; bad=1 ;; esac
fi
finish
