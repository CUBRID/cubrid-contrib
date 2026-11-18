#!/bin/bash
# t17: picking the server (no CUBRID server).  Stand-in "cub_server"
# processes (a program that only waits, under that name): A and B whose
# numbers cross (A's db name is B's pid, B's is A's), C with a numeric db
# name, D and E serving one db name.  A bare target is a db name first:
# one server runs (a numeric name also another server's pid is noted, the
# name wins), several are refused with the --pid of each; a number that
# names no db is a pid.  --pid picks by pid only and refuses a pid that is
# not a cub_server.  A stand-in that is picked ends at "libcubrid.so not
# mapped": past the choice, nothing of it is read.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
T=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "t17 FAIL: $*"; exit 1; }
run() { LANG=C timeout 10 "$TOOL" "$@" 2>&1; }
picked() { echo "$1" | grep -q "libcubrid.so not mapped"; }

printf "#include <unistd.h>\nint main (void) { pause (); return 0; }\n" > "$T/w.c"
gcc -o "$T/cub_server" "$T/w.c" || fail "build of the stand-in"
mkfifo "$T/a"
# A waits for B's pid, then becomes "cub_server <B's pid>" (same pid)
bash -c 'read -r x < "$1"; exec "$2" "$x"' _ "$T/a" "$T/cub_server" &
A=$!; PIDS+=("$A")
"$T/cub_server" "$A" &
B=$!; PIDS+=("$B")
echo "$B" > "$T/a"
C_DB=999999999
"$T/cub_server" "$C_DB" &
C=$!; PIDS+=("$C")
"$T/cub_server" t17dup &
D=$!; PIDS+=("$D")
"$T/cub_server" t17dup &
E=$!; PIDS+=("$E")
for _ in $(seq 50); do
  [ "$(cat /proc/$A/comm 2>/dev/null)" = cub_server ] && break
  sleep 0.1
done

L=$(run -l)
echo "$L" | grep -Eq "^ +$A +$B\$" || fail "A not listed as db $B: $L"
echo "$L" | grep -Eq "^ +$B +$A\$" || fail "B not listed as db $A: $L"

# crossed numbers: the db name wins, the other server's pid is noted
O=$(run "$A")
echo "$O" | grep -q "'$A' is a db name and the pid of another cub_server: using db $A (pid $B); for pid $A use --pid $A" \
  || fail "'$A' not taken as db $A of pid $B: $O"
picked "$O" || fail "'$A': nothing picked: $O"
O=$(run "$B")
echo "$O" | grep -q "using db $B (pid $A); for pid $B use --pid $B" \
  || fail "'$B' not taken as db $B of pid $A: $O"

# one db name, two servers: refused, each --pid offered
O=$(run t17dup); rc=$?
[ $rc -eq 1 ] || fail "duplicate db name: exit $rc"
picked "$O" && fail "duplicate db name: a server was picked: $O"
echo "$O" | grep -q "db 't17dup' is served by 2 cub_server processes; choose one with --pid:" \
  || fail "duplicate db name: no refusal: $O"
echo "$O" | grep -q "^  cub_ctv --pid $D\$" || fail "--pid $D not offered: $O"
echo "$O" | grep -q "^  cub_ctv --pid $E\$" || fail "--pid $E not offered: $O"

# a numeric db name that is no pid; a pid that is no db name
O=$(run "$C_DB")
picked "$O" || fail "numeric db name $C_DB not selected: $O"
echo "$O" | grep -q "pid of another" && fail "numeric db name $C_DB noted: $O"
O=$(run "$C")
picked "$O" || fail "pid $C not selected: $O"

# --pid: by pid only
for n in "$A" "$B" "$D"; do
  O=$(run --pid "$n")
  picked "$O" || fail "--pid $n did not pick pid $n: $O"
  echo "$O" | grep -q "pid of another\|choose one" && fail "--pid $n read as a db name: $O"
done
O=$(run --pid "$C" --detail LOG)
picked "$O" || fail "--pid with a --detail word: $O"
O=$(run --pid $$)
echo "$O" | grep -q "pid $$ is not a cub_server process" || fail "--pid of a non-server: $O"
O=$(run --pid 0)
echo "$O" | grep -q "needs a positive pid" || fail "--pid 0: $O"
O=$(run --pid "$A" demodb)
echo "$O" | grep -q "target already given" || fail "--pid plus a target: $O"

echo "t17 PASS: a target is a db name first (several servers refused, each --pid offered); a number naming no db is a pid; --pid picks by pid"
