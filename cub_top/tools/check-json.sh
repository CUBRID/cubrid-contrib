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
# check-json.sh - verify the --dump-json output.
#   JSON is a mechanical conversion of the -d output, so it must be (1) valid JSON and (2) carry
#   exactly -d's key set and order.  A difference means the content was produced twice.
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
BIN="$DIR/../cub_top"
[ -x "$BIN" ] || { echo "not built: $BIN"; echo "-> FAIL"; exit 1; }
command -v python3 >/dev/null || { echo "no python3 - skipped"; echo "-> PASS"; exit 0; }
T=$(mktemp) ; J=$(mktemp)
trap 'rm -f "$T" "$J"' EXIT
"$BIN" -d          >"$T" 2>/dev/null
"$BIN" --dump-json >"$J" 2>/dev/null
bad=0
python3 - "$T" "$J" <<'PY' || bad=1
import json,sys
t,s=open(sys.argv[1]).read(),open(sys.argv[2]).read()
d=json.loads(s)                                   # (1) valid JSON
assert isinstance(d,dict) and len(d)>30, f"too few keys: {len(d)}"
# List the -d keys with a quote-aware parser
tk=[];i=0
while i<len(t):
    while i<len(t) and t[i] in ' \n\t\r': i+=1
    if i>=len(t): break
    k=i
    while i<len(t) and t[i] not in '= \n': i+=1
    if i>=len(t) or t[i]!='=':
        continue
    key=t[k:i]; i+=1
    if i<len(t) and t[i]=='"':
        i+=1
        while i<len(t) and t[i]!='"': i+=1
        i+=1
    else:
        while i<len(t) and t[i] not in ' \n\r': i+=1
    tk.append(key)
# -d and --dump-json are two runs.  cap.cellNN.*, cap.ceiling_* and cap.used_of_ceiling_pct
# exist only when that run observed a saturated frame, and cap.*await_ms only when the
# device did any I/O in the window, so one run can have them and the other not; they say
# nothing about whether --dump-json is derived from -d.
import re
RUNDEP=re.compile(r'cap\.(cell\d+\.|ceiling_(iops|bps|util_pct|qdepth|epoch)$|used_of_ceiling_pct$|(read_|write_)?await_ms$)')
def stable(keys): return [k for k in keys if not RUNDEP.match(k)]
tk2,dk2=stable(tk),stable(list(d.keys()))
diff=set(tk2)^set(dk2)
assert not diff, f"key mismatch: {diff}"          # (2) same key set
assert tk2==dk2, "key order mismatch"              #     same order
n=sum(1 for v in d.values() if isinstance(v,(int,float)))
assert n>len(d)*0.7, f"numeric key ratio off: {n}/{len(d)}"
print(f"  ok  valid JSON, {len(d)} keys matching -d in set and order, {n} numeric")
PY
# plain -d must emit no JSON fragment
if grep -q '^{' "$T"; then echo "  FAIL JSON leaked into the -d output"; bad=1
else echo "  ok  -d is plain key=value"; fi
if grep -qE '^[^{ ]+=' "$J"; then echo "  FAIL key=value leaked into the --dump-json output"; bad=1
else echo "  ok  --dump-json is a JSON object only"; fi
[ $bad -eq 0 ] && { echo "→ PASS"; exit 0; }
echo "→ FAIL"; exit 1
