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
# The incremental refresh (-i: r, auto tick) must reach the state a fresh run reaches,
# while reading as little as possible:
#   1. with nothing changed, every file is re-applied from its remembered sector list
#      (header LSA unchanged) and the tracker walk is skipped; the report is identical;
#   2. after a table is created and filled in the database, the refresh re-walks the
#      files whose header LSA moved, walks the tracker again, and the report equals a
#      fresh run taken after the change.
# Uses the VOLMAP_REFRESH_LOOPS / VOLMAP_REFRESH_SLEEP test hook of the batch run.
# Needs a running database RUNDB (default demodb) that csql's dba can change without a
# password; the online backup it takes to force a checkpoint leaves an entry in the
# database's _bkvinf (the backup files themselves are deleted).
#
#   RUNDB=demodb sh tools/check-refresh.sh
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
BIN=${VOLMAP:-$ROOT/cub_volmap}
RUNDB=${RUNDB:-demodb}
TMP=$(mktemp -d /tmp/volmap-refresh.XXXXXX)
vp=""; T=""
# whatever is left when this script ends or is killed: the paused volmap, the test table
cleanup(){ [ -n "$vp" ] && kill "$vp" 2>/dev/null; [ -n "$T" ] && csql -u dba "$RUNDB" -c "DROP TABLE IF EXISTS $T" >/dev/null 2>&1; rm -rf "$TMP"; }
# a signal must end the script, not just clean up and let it run on
trap cleanup EXIT
trap 'cleanup; trap - EXIT; exit 143' TERM
trap 'cleanup; trap - EXIT; exit 130' INT
rc=0
[ -x "$BIN" ] || { echo "→ FAIL (no binary: $BIN)"; exit 1; }
command -v csql >/dev/null || { echo "→ SKIP (no csql in PATH)"; exit 0; }
same(){ python3 - "$1" "$2" <<'PY'
import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
def key(d):
    return (sorted((f["vfid"], f["type"], f["sectors"], f["alloc_pages"]) for f in d["files"]),
            sorted((v["volid"], v["sectors_reserved"], v["pages_allocated"], v["unknown_sectors"]) for v in d["volumes"]))
ka, kb = key(a), key(b)
if ka != kb:
    fa, fb = set(map(str, ka[0])), set(map(str, kb[0]))
    print("   files only in first: %s; only in second: %s" % (sorted(fa - fb)[:4], sorted(fb - fa)[:4]))
    print("   volumes: %s vs %s" % (ka[1], kb[1]))
sys.exit(0 if ka == kb else 1)
PY
}
# 1. nothing changed: three refreshes, all from cache, tracker skipped, same report
"$BIN" --format=json "$RUNDB" > "$TMP/a.json" 2>/dev/null || { echo "→ SKIP ($RUNDB not readable)"; exit 0; }
if [ "$(python3 -c "import json; print(json.load(open('$TMP/a.json'))['server']['state'])")" != running ]; then
  echo "→ SKIP ($RUNDB has no running server on this host)"; exit 0; fi
VOLMAP_REFRESH_LOOPS=3 "$BIN" --format=json "$RUNDB" > "$TMP/b.json" 2> "$TMP/b.err"
# files on temporary-purpose volumes carry the constant temp LSA, so they are never
# served from the cache: they are re-walked every time, and that is the expected count
read nf nt <<EOT
$(python3 -c "
import json; d=json.load(open('$TMP/a.json'))
tv={v['volid'] for v in d['volumes'] if v['purpose']=='temporary'}
nt=sum(1 for f in d['files'] if int(f['vfid'].split('|')[0]) in tv)
print(len(d['files'])-nt, nt)")
EOT
if same "$TMP/a.json" "$TMP/b.json" && [ "$(grep -c "refresh [123]: $nf files from cache, $nt re-walked, tracker unchanged, skipped" "$TMP/b.err")" = 3 ]; then
  echo "  ok     unchanged database: 3 refreshes re-applied $nf files from cache ($nt on temporary volumes re-walked, as they must be), tracker walk skipped, report identical"
else echo "  FAIL   unchanged database (want $nf from cache, $nt re-walked):"; cat "$TMP/b.err"; rc=1; fi
# 2. a table created and filled during the pause: the refresh must reach the fresh state
T=volmap_rt_$$
cat > "$TMP/mk.sql" <<SQL
CREATE TABLE $T (a INT PRIMARY KEY, b VARCHAR(2000));
INSERT INTO $T VALUES (1, REPEAT('x', 1500));
INSERT INTO $T SELECT a + 1, b FROM $T;
INSERT INTO $T SELECT a + 2, b FROM $T;
INSERT INTO $T SELECT a + 4, b FROM $T;
INSERT INTO $T SELECT a + 8, b FROM $T;
INSERT INTO $T SELECT a + 16, b FROM $T;
INSERT INTO $T SELECT a + 32, b FROM $T;
INSERT INTO $T SELECT a + 64, b FROM $T;
INSERT INTO $T SELECT a + 128, b FROM $T;
INSERT INTO $T SELECT a + 256, b FROM $T;
INSERT INTO $T SELECT a + 512, b FROM $T;
INSERT INTO $T SELECT a + 1024, b FROM $T;
COMMIT;
SQL
# The new pages sit in the server's buffer until a checkpoint (none for minutes on an
# idle server), and a refresh reads the disk.  An online backup forces one: the cheapest
# way to put the change on disk from outside.
VOLMAP_REFRESH_LOOPS=1 VOLMAP_REFRESH_SLEEP=40 "$BIN" --format=json "$RUNDB" > "$TMP/c.json" 2> "$TMP/c.err" & vp=$!
sleep 2
csql -u dba "$RUNDB" -i "$TMP/mk.sql" > "$TMP/csql.out" 2>&1 || { echo "  FAIL   csql: $(tail -2 "$TMP/csql.out")"; rc=1; }
mkdir -p "$TMP/bk"; cubrid backupdb -C -D "$TMP/bk" "$RUNDB" > "$TMP/bk.out" 2>&1 || { echo "  FAIL   backupdb (to force a checkpoint): $(tail -1 "$TMP/bk.out")"; rc=1; }
rm -rf "$TMP/bk"
wait $vp; vp=""
"$BIN" --format=json "$RUNDB" > "$TMP/d.json" 2>/dev/null
if same "$TMP/c.json" "$TMP/d.json" && grep -q "tracker re-walked" "$TMP/c.err" && ! grep -q " 0 re-walked" "$TMP/c.err"; then
  echo "  ok     table created and filled during the pause: refresh equals a fresh run ($(cat "$TMP/c.err"))"
else echo "  FAIL   changed database: $(cat "$TMP/c.err")"; rc=1; fi
# 3. the table dropped during the pause: its files leave the tracker (the header may stay
#    on disk until vacuum frees the sectors), so the refresh must show them as not tracked
#    - or gone - never as tracked
# the table's files by class name - schema-qualified on 11.2+ (owner.table), so only the
# last part is compared (a VFID of an earlier dropped table may be reused, so
# "not in the first report" would miss them)
newf=$(python3 -c "
import json
print(' '.join(f['vfid'] for f in json.load(open('$TMP/d.json'))['files'] if f['class'].lower().split('.')[-1] == '$T'.lower()))")
if [ -z "$newf" ]; then echo "  FAIL   the table's files were not found in the fresh report (class $T)"; rc=1; else
VOLMAP_REFRESH_LOOPS=1 VOLMAP_REFRESH_SLEEP=40 "$BIN" --format=json "$RUNDB" > "$TMP/e.json" 2> "$TMP/e.err" & vp=$!
sleep 2
csql -u dba "$RUNDB" -c "DROP TABLE $T" > /dev/null 2>&1; T=""
mkdir -p "$TMP/bk"; cubrid backupdb -C -D "$TMP/bk" "$RUNDB" > "$TMP/bk.out" 2>&1; rm -rf "$TMP/bk"
wait $vp; vp=""
st=$(python3 -c "
import json; d={f['vfid']: f['tracked'] for f in json.load(open('$TMP/e.json'))['files']}
print(' '.join('%s=%s' % (v, d.get(v, 'gone')) for v in '$newf'.split()))")
case " $st " in
  *"=True "*) echo "  FAIL   dropped table's files still tracked after the refresh: $st"; rc=1 ;;
  *) echo "  ok     table dropped during the pause: its files are no longer tracked ($st)" ;;
esac
fi
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
