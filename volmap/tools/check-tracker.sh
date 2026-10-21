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
# The file tracker gives the same files and the same sector ownership as probing every
# reserved sector's first page, for a fraction of the reads.  For each database given
# (names or vinf paths; default: every database in $CUBRID_DATABASES/databases.txt)
# the JSON of a default run and of --no-tracker are compared: the set of files
# (vfid, type), each volume's reserved/unknown sector counts and the findings must be
# identical; the tracker run must have read the tracker and probed no more sectors.
#
#   sh tools/check-tracker.sh [db-or-vinf ...]
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
BIN=${VOLMAP:-$ROOT/cub_volmap}
TMP=$(mktemp -d /tmp/volmap-tracker.XXXXXX); trap 'rm -rf "$TMP"' EXIT
[ -x "$BIN" ] || { echo "→ FAIL (no binary: $BIN)"; exit 1; }
if [ $# -gt 0 ]; then DBS="$*"; else
  DBS=$(grep -v '^#' "${CUBRID_DATABASES:-$CUBRID/databases}/databases.txt" 2>/dev/null | awk 'NF>=2{print $1}')
fi
[ -n "$DBS" ] || { echo "→ SKIP (no database to test)"; exit 0; }
rc=0
for db in $DBS; do
  "$BIN" --format=json "$db" > "$TMP/t.json" 2>"$TMP/t.err"; r1=$?
  "$BIN" --format=json --no-tracker "$db" > "$TMP/p.json" 2>"$TMP/p.err"; r2=$?
  if [ $r1 -ne 0 ] && [ $r1 -ne 2 ] && [ $r1 -ne 3 ]; then echo "  SKIP   $db: exit $r1 ($(head -1 "$TMP/t.err"))"; continue; fi
  if [ $r2 -ne 0 ] && [ $r2 -ne 2 ] && [ $r2 -ne 3 ]; then echo "  FAIL   $db: --no-tracker exit $r2 ($(head -1 "$TMP/p.err"))"; rc=1; continue; fi
  python3 - "$TMP/t.json" "$TMP/p.json" "$db" <<'PY' || rc=1
import json, sys
t = json.load(open(sys.argv[1])); p = json.load(open(sys.argv[2])); db = sys.argv[3]
def key(d):
    files = sorted((f["vfid"], f["type"], f["sectors"], f["alloc_pages"]) for f in d["files"])
    vols = sorted((v["volid"], v["sectors_reserved"], v["unknown_sectors"], v["pages_allocated"]) for v in d["volumes"])
    finds = sorted(json.dumps(f, sort_keys=True) for f in d["findings"])
    return files, vols, finds
kt, kp = key(t), key(p)
dt, dp = t["discovery"], p["discovery"]
ok = kt == kp and dt["tracker"] == "read" and dp["tracker"] == "off" and dt["probed_sectors"] <= dp["probed_sectors"]
untracked = sum(1 for f in t["files"] if f["tracked"] is False)
deleted = sum(1 for f in t["files"] if f["tracked"] == "deleted")
print("  %s   %s: %d files (tracker %d, not listed %d, deleted %d), sectors probed %d vs %d, same files/sectors/findings: %s%s"
      % ("ok  " if ok else "FAIL", db, len(t["files"]), dt["tracker_files"], untracked, deleted,
         dt["probed_sectors"], dp["probed_sectors"], kt == kp, "" if dt["tracker"] == "read" else " [tracker %s: %s]" % (dt["tracker"], dt["reason"])))
if kt != kp:
    for a, b, what in zip(kt, kp, ("files", "volumes", "findings")):
        if a != b:
            sa, sb = set(map(str, a)), set(map(str, b))
            print("        %s differ: only with tracker %s; only probed %s" % (what, sorted(sa - sb)[:5], sorted(sb - sa)[:5]))
sys.exit(0 if ok else 1)
PY
done
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
