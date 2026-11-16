#!/bin/bash
# t7: on a live server, what --record wrote must replay to exactly what was
# shown (snapshot and live windows), and the detail dump must be valid JSON
# that --replay refuses.  usage: t7_record_equiv.sh <db> [live seconds]
set -u
DB=${1:?db name}
SECS=${2:-6}
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/cub_ctv}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export COLUMNS=120 LANG=C
fail () { echo "t7 FAIL: $1"; exit 1; }
# DB may be a pid (several servers of one name); shared resolver
ENV=$(CUB_CTV_TOOL="$TOOL" python3 "$HERE/tests/server_env.py" "$DB") || fail "no cub_server for $DB"
eval "$ENV"

"$TOOL" "$PID" --record "$T/s.rec" > "$T/s.live" 2>&1 || fail "snapshot record"
"$TOOL" --replay "$T/s.rec" > "$T/s.replay" 2>&1 || fail "snapshot replay"
cmp -s "$T/s.live" "$T/s.replay" || fail "snapshot replay differs"

timeout -s INT "$SECS" "$TOOL" "$PID" -i --record "$T/l.rec" > "$T/l.live" 2>&1
"$TOOL" --replay "$T/l.rec" --replay-speed 0 > "$T/l.replay" 2>&1 || fail "live replay"
cmp -s "$T/l.live" "$T/l.replay" || fail "live replay differs"

"$TOOL" "$PID" --dump "$T/d.json" > /dev/null 2>&1 || fail "detail dump"
python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['replayable'] is False and len(d['cs'])>0" "$T/d.json" \
  || fail "detail dump is not the expected JSON"
"$TOOL" --replay "$T/d.json" > /dev/null 2>&1 && fail "--replay accepted a detail dump"
echo "t7 PASS: snapshot and $(grep -c '^F' "$T/l.rec") live windows replay identically; detail dump $(wc -c < "$T/d.json") B refused by --replay"
