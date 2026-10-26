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
# --stream: the live loop without a terminal, one JSON frame per interval on stdout - the
# recording format, so a collector reads it as it reads a recording - until INT/TERM or the
# consumer closing the pipe (exit 0 either way), with nothing meant for a terminal in it.
# Needs a running cub_server.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR"); BIN="$ROOT/cub_top"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-stream.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
rc=0
"$BIN" -d 2>/dev/null | grep -q '^server.up=1' || { echo "→ SKIP (no running cub_server)"; exit 0; }
"$BIN" --stream --interval 0.3 </dev/null >"$TMP/s.jsonl" 2>"$TMP/s.err" & p=$!
sleep 2.5; kill -TERM $p; wait $p; e=$?
n=$(grep -c '^{' "$TMP/s.jsonl"); esc=$(grep -c "$(printf '\033')" "$TMP/s.jsonl")
if [ "$e" = 0 ] && [ "$n" -ge 4 ] && [ "$esc" = 0 ] && [ ! -s "$TMP/s.err" ]; then echo "  ok     SIGTERM after 2.5 s: exit 0, $n frames, no escape sequence, stderr empty"
else echo "  FAIL   SIGTERM: exit $e, $n frames, $esc escapes, stderr: $(head -c 80 "$TMP/s.err")"; rc=1; fi
python3 - "$TMP/s.jsonl" <<'PY' || rc=1
import json,sys
L=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
keys=[set(f) for f in L]
ok = len(L)>=2 and all(k==keys[1] for k in keys[1:]) and all(f.get("ts_epoch_ms",0)>0 for f in L) and L[1]["ts_epoch_ms"]>L[0]["ts_epoch_ms"]
print("  %s   every line parses; frames 2.. carry the same key set (%d keys), times increase"%("ok  " if ok else "FAIL",len(keys[1]) if len(keys)>1 else 0))
sys.exit(0 if ok else 1)
PY
# The same keys as a recording made by this build (the stream is the recording format)
"$BIN" -b --interval 0.3 --record "$TMP/r.jsonl" </dev/null >/dev/null 2>&1 & p=$!; sleep 1.5; kill -TERM $p; wait $p 2>/dev/null
python3 - "$TMP/s.jsonl" "$TMP/r.jsonl" <<'PY' || rc=1
import json,sys
a=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]; b=[json.loads(l) for l in open(sys.argv[2]) if l.strip()]
ka=set(a[-1]); kb=set(b[-1]); d=ka^kb
ok = len(b)>=1 and not d
print("  %s   same keys as --record (%d); differing: %s"%("ok  " if ok else "FAIL",len(ka),sorted(d)[:6]))
sys.exit(0 if ok else 1)
PY
# The consumer closes the pipe: the run ends promptly with status 0
t0=$(date +%s.%N)
( "$BIN" --stream --interval 0.2 </dev/null 2>/dev/null; echo $? > "$TMP/pst" ) | head -2 > "$TMP/h.jsonl"
t1=$(date +%s.%N); el=$(python3 -c "print(round($t1-$t0,1))"); ps=$(cat "$TMP/pst")
if [ "$ps" = 0 ] && [ "$(grep -c '^{' "$TMP/h.jsonl")" = 2 ] && python3 -c "import sys; sys.exit(0 if $el<4 else 1)"; then echo "  ok     pipe closed by the consumer after 2 frames: exit 0 within ${el}s"
else echo "  FAIL   pipe closed: exit $ps after ${el}s, $(grep -c '^{' "$TMP/h.jsonl") frames"; rc=1; fi
# Combinations refused with status 2
for o in "--stream --record $TMP/x" "--stream -d" "--replay $ROOT/examples/10-recording.jsonl --stream"; do
    "$BIN" $o </dev/null >/dev/null 2>"$TMP/oerr"; e=$?
    if [ "$e" = 2 ]; then echo "  ok     $o: refused ($(head -1 "$TMP/oerr" | cut -c1-50))"; else echo "  FAIL   $o: exit $e"; rc=1; fi
done
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
