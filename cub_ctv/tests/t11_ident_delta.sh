#!/bin/bash
# t11: identity deltas in a record (K/I/X lines) - no server needed.  From
# the live fixture, two records with the same synthetic identity sets are
# made: one listing every identity in every frame (no K lines), one written
# as keyframes plus changes and removals.  Both must replay to the same
# transaction tables, and in the delta record every frame reached by a seek
# (backward, random) must carry the same identities as in sequential order.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail () { echo "t11 FAIL: $1"; exit 1; }

python3 - "$HERE/fixtures/demodb-live.rec" "$T" <<'PY' || fail "make records"
import sys
src, out = sys.argv[1], sys.argv[2]
L = open(src).read().split("\n")
base = []
for l in L:
    if l.startswith("I "):
        base.append(l.split(" "))
    if l == "E":
        break
def ident_set(k):
    s = {}
    for t in base:
        idx = int(t[1])
        if (idx + k) % 5 == 0:
            continue                      # gone in this frame
        t = list(t)
        if (idx * (k + 1)) % 7 == 1:
            t[5] = t[5] + "x%d" % k       # changed in this frame
        s[idx] = " ".join(t)
    return s
full, delta, k, prev = [], [], -1, {}
for l in L:
    if l.startswith("I "):
        continue
    full.append(l); delta.append(l)
    if l.startswith("B "):
        k += 1
        cur = ident_set(k)
        full.extend(cur[i] for i in sorted(cur))
        key = k % 4 == 0
        delta.append("K %d" % key)
        for i in sorted(cur):
            if key or prev.get(i) != cur[i]:
                delta.append(cur[i])
        if not key:
            delta.extend("X %d" % i for i in sorted(prev) if i not in cur)
        prev = cur
open(out + "/full.rec", "w").write("\n".join(full))
open(out + "/delta.rec", "w").write("\n".join(delta))
PY
read -r NF NX NK0 < <(python3 -c "
L=open('$T/delta.rec').read().split('\n')
print(sum(l.startswith('F ') for l in L), sum(l.startswith('X ') for l in L), sum(l=='K 0' for l in L))")
[ "$NX" -gt 0 ] && [ "$NK0" -gt 0 ] || fail "synthetic record has no removals or delta frames"
"$TOOL" --replay "$T/full.rec" --trans --replay-speed 0 > "$T/full.out" 2>&1 || fail "replay full"
"$TOOL" --replay "$T/delta.rec" --trans --replay-speed 0 > "$T/delta.out" 2>&1 || fail "replay delta"
cmp -s "$T/full.out" "$T/delta.out" || { diff "$T/full.out" "$T/delta.out" | head; fail "delta replay differs"; }

mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v '/cub_ctv\.c$')
gcc -O1 -Wall -I"$HERE/../src" -o "$T/seek" "$HERE/t11_seek.c" "${SRCS[@]}" || fail "build seek check"
"$T/seek" "$T/delta.rec" || fail "seek"
echo "t11 PASS: $NF frames ($NK0 delta, $NX removals) replay as the full record; seeks match sequential"
