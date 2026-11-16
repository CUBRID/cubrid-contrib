#!/bin/bash
# t5_replay.sh - offline regression: replay the recorded fixtures (--record
# files) and compare with the stored golden text, byte for byte: snapshot,
# --trans, --detail, --narrow of the snapshot record; live and live --detail
# --narrow of the live record (English).  --json output is only parsed, not
# compared.  No server and no /proc access.  Regenerate goldens with --update.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/cub_ctv}
FIX=$HERE/tests/fixtures
rc=0
check () {                      # name, golden, replay options...
  local name=$1 gold=$2; shift 2
  local out
  out=$(env -u COLUMNS LANG=C TZ=UTC "$TOOL" "$@" --en 2>&1)
  if [ "${UPDATE:-0}" = 1 ]; then printf '%s\n' "$out" > "$gold"; echo "t5 UPDATED: $name"; return; fi
  if printf '%s\n' "$out" | diff -q - "$gold" >/dev/null; then
    echo "t5 PASS: $name"
  else
    echo "t5 FAIL: $name differs from golden"; printf '%s\n' "$out" | diff - "$gold" | head -5; rc=1
  fi
}
[ "${1:-}" = "--update" ] && UPDATE=1
s=$FIX/demodb-snapshot.rec
l=$FIX/demodb-live.rec
check snapshot        "$s.snapshot.golden" --replay "$s"
check snapshot-trans  "$s.trans.golden"    --replay "$s" --trans
check snapshot-detail "$s.detail.golden"   --replay "$s" --detail
check snapshot-narrow "$s.narrow.golden"   --replay "$s" --narrow
check live            "$l.live.golden"     --replay "$l" --replay-speed 0
check live-detail     "$l.detail.golden"   --replay "$l" --replay-speed 0 --detail --narrow
set -o pipefail
for f in "$s" "$l"; do
  env -u COLUMNS "$TOOL" --replay "$f" --replay-speed 0 --json \
    | python3 -c "import sys,json; n=[json.loads(x) for x in sys.stdin if x.strip()]; sys.exit(0 if n else 1)" \
    && echo "t5 PASS: $(basename "$f") json" || { echo "t5 FAIL: $(basename "$f") json"; rc=1; }
done
exit $rc
