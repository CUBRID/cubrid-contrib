#!/bin/bash
# t10_os_probe.sh - OS thread mapping (I-17) and wait reasons (I-18) against
# /proc on a live server:
#  (1) every entry's os_tid in a --dump is a task of the target, all distinct,
#      and their number is within THR_ID_SLACK (8) of the task count less one;
#  (2) thread names come from /proc/<pid>/task/<tid>/comm;
#  (3) every entry's resume_status is a code 0..27 and the snapshot prints
#      the "thread waits" line.
# usage: t10_os_probe.sh [dbname]
set -u
DB=${1:-demodb}
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/cub_ctv}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail () { echo "t10 FAIL: $1"; exit 1; }
# DB may be a pid (several servers of one name); shared resolver
ENV=$(CUB_CTV_TOOL="$TOOL" python3 "$HERE/tests/server_env.py" "$DB") || fail "no cub_server for $DB"
eval "$ENV"

"$TOOL" "$PID" --dump "$T/d.json" > "$T/snap.txt" 2>&1 || fail "dump: $(tail -1 "$T/snap.txt")"
python3 - "$T/d.json" "$T/snap.txt" <<'PY' || exit 1
import json, os, sys
d = json.load(open(sys.argv[1])); snap = open(sys.argv[2]).read()
t = d["threads"]; pid = d["target"]["pid"]
if not t["has_tid"]:
    print("t10 FAIL: I-17 not probed (%s)" % d["degraded_features"]); sys.exit(1)
tasks = set(int(x) for x in os.listdir("/proc/%d/task" % pid))
ents = [e for e in t["entries"] if "os_tid" in e]
tids = [e["os_tid"] for e in ents]
if not tids or any(x not in tasks for x in tids) or len(set(tids)) != len(tids):
    print("t10 FAIL: os_tid not a distinct task set"); sys.exit(1)
if abs(len(tids) - (len(tasks) - 1)) > 8:
    print("t10 FAIL: %d tids vs %d tasks" % (len(tids), len(tasks))); sys.exit(1)
bad = [e for e in ents if open("/proc/%d/task/%d/comm" % (pid, e["os_tid"])).read().strip() != e["comm"]]
if bad:
    print("t10 FAIL: comm mismatch %s" % bad[:2]); sys.exit(1)
if not t["has_resume"]:
    print("t10 FAIL: I-18 not probed (%s)" % d["degraded_features"]); sys.exit(1)
codes = [e["resume_status"] for e in ents]
if any(c < 0 or c > 27 for c in codes):
    print("t10 FAIL: resume code out of range"); sys.exit(1)
if "thread waits:" not in snap and "스레드 대기:" not in snap:
    print("t10 FAIL: snapshot lacks the thread waits line"); sys.exit(1)
names = sorted(set(e["comm"] for e in ents))
print("t10 PASS: %d os tids == /proc tasks, names %s..., resume codes %s" % (len(tids), ",".join(names[:4]), sorted(set(codes))))
PY
