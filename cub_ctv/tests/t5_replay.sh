#!/bin/bash
# t5_replay.sh - offline regression: replay the recorded fixtures (--record
# files) and compare with the stored golden text, byte for byte: snapshot,
# --trans, --detail, --narrow of the snapshot record; live and live --detail
# --narrow of the live record (English).  --json output is only parsed, not
# compared.  Also: Korean messages only on a UTF-8 locale (LC_ALL/LC_MESSAGES/
# LC_CTYPE/LANG), otherwise English with one warning; English output is ASCII
# only.  No server and no /proc access.  Regenerate goldens with --update.
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
# a real number token that is nan / inf is refused with its line (JSON has
# neither): H interval, F dt, C total/max/prev total, D await/util, G max_ms
bad=$(mktemp); trap 'rm -f "$bad"' EXIT
nonfinite () {   # file, awk edit, label
  awk "$2" "$1" > "$bad"
  ln=$(awk "$2" "$1" | diff - "$1" | sed -n 's/^\([0-9]*\)c.*/\1/p' | head -1)
  out=$(env -u COLUMNS "$TOOL" --replay "$bad" --replay-speed 0 --json 2>&1 >/dev/null)
  case "$out" in
    *"malformed record file at $bad:$ln") ;;
    *) echo "t5 FAIL: $3 accepted or wrong line: $out"; rc=1; return ;;
  esac
}
nonfinite "$l" '$1=="H"&&!d{$5="inf";d=1}{print}' "H interval inf"
nonfinite "$l" '$1=="F"&&!d{$5="nan";d=1}{print}' "F dt nan"
nonfinite "$s" '$1=="C"&&!d{$13="nan";d=1}{print}' "C total_ms nan"
nonfinite "$s" '$1=="C"&&!d{$14="inf";d=1}{print}' "C max_ms inf"
nonfinite "$l" '$1=="C"&&!d{$23="-inf";d=1}{print}' "C prev total_ms -inf"
nonfinite "$l" '$1=="B"&&!d{$0="D nan 1.0 sda";d=1}{print}' "D await nan"
nonfinite "$l" '$1=="B"&&!d{$0="D 1.0 inf sda";d=1}{print}' "D util inf"
nonfinite "$s" '$1=="G"&&!d{$5="nan";d=1}{print}' "G max_ms nan"
[ $rc -eq 0 ] && echo "t5 PASS: nan / inf real tokens refused with their line"
# Korean only on a UTF-8 locale: otherwise English (ASCII only) and one warning
lbad=0
locale_case () {                # expect (ko|en), env assignments..., [-- args]
  local want=$1 out warned; shift
  local envs=() args=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
  [ "${1:-}" = "--" ] && shift && args=("$@")
  out=$(env -i PATH="$PATH" TZ=UTC "${envs[@]}" "$TOOL" --replay "$s" "${args[@]}" 2>&1)
  case "$out" in *"charset is not UTF-8"*) warned=1 ;; *) warned=0 ;; esac
  if [ "$want" = ko ]; then
    case "$out" in *"스레드 대기"*) [ $warned = 0 ] && return ;; esac
  elif ! printf '%s' "$out" | LC_ALL=C grep -q '[^ -~[:space:]]' && [ $warned = 1 ]; then
    return
  fi
  echo "t5 FAIL: locale ${envs[*]} ${args[*]}: expected $want"; rc=1; lbad=1
}
locale_case ko LANG=ko_KR.UTF-8
locale_case ko LANG=ko_KR.utf8
locale_case ko LANG=en_US.UTF-8 LC_MESSAGES=ko_KR.UTF-8
locale_case ko LANG=C -- --ko
locale_case en LANG=ko_KR.eucKR
locale_case en LANG=ko_KR
locale_case en LANG=ko_KR.UTF-8 LC_ALL=ko_KR.euckr
locale_case en LANG=ko_KR.eucKR -- --ko
for g in "$FIX"/*.golden; do
  LC_ALL=C grep -q '[^ -~[:space:]]' "$g" && { echo "t5 FAIL: English output not ASCII: $g"; rc=1; lbad=1; }
done
[ $lbad -eq 0 ] && echo "t5 PASS: Korean only on UTF-8 locales; English output is ASCII"
exit $rc
