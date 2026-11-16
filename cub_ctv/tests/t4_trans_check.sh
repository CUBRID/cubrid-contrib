#!/bin/bash
# t4_trans_check.sh - regression for the transaction-identity view.
#
#  (1) --trans must never print a released slot: no row with trid -1.
#  (2) with a live csql client (when the server accepts connections),
#      a row with program csql and a positive trid (or "-") must appear,
#      and csql must also be listed by cubrid tranlist (compared only when
#      tranlist prints something).
#  (2) is SKIPped when csql cannot connect (e.g. orphaned master); then
#      the output must show the "no client" marker or some client row.
#
# usage: t4_trans_check.sh [dbname]
set -u
DB=${1:-demodb}
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOOL=$(readlink -f "${CUB_CTV_TOOL:-$HERE/cub_ctv}")
fail () { echo "t4 FAIL: $1"; exit 1; }
# csql writes csql.err into the cwd: run from a scratch directory
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT; cd "$WORK"

# DB may be a pid (several servers of one name); shared resolver
ENV=$(CUB_CTV_TOOL="$TOOL" python3 "$HERE/tests/server_env.py" "$DB") || fail "no cub_server for $DB"
eval "$ENV"
export CUBRID=$INSTALL CUBRID_DATABASES=$DBDIR LD_LIBRARY_PATH=$INSTALL/lib

OUT=$("$TOOL" "$PID" --trans) || fail "--trans exited nonzero"
# data rows start with a numeric idx; never couple to preamble line counts
echo "$OUT" | awk '$1 ~ /^[0-9]+$/ && $2 == -1 {exit 1}' \
  || fail "released slot (trid -1) printed"

if timeout 10 "$INSTALL/bin/csql" -u dba -C "$DB" -c "SELECT 1;" \
     >/dev/null 2>&1; then
  timeout 15 "$INSTALL/bin/csql" -u dba -C "$DB" -c "SELECT SLEEP(8);" \
    >/dev/null 2>&1 &
  sleep 3
  OUT=$("$TOOL" "$PID" --trans)
  # trid is a positive number, or "-" when the tool could not validate
  # the trid offset (supported degraded mode)
  echo "$OUT" | awk '$1 ~ /^[0-9]+$/ && $4 == "csql" && ($2 > 0 || $2 == "-") {found=1}
                     END {exit !found}' \
    || { wait; fail "live csql client not listed"; }
  # tranlist: the same client must appear in cubrid tranlist
  TL=$(echo "" | timeout 10 "$INSTALL/bin/cubrid" tranlist "$DB" 2>/dev/null)
  if [ -n "$TL" ]; then
    echo "$TL" | grep -q "csql" || { wait; fail "tranlist disagrees"; }
  fi
  wait
  echo "t4 PASS (idle filter + live client + tranlist)"
else
  if ! echo "$OUT" | grep -q "no client\|클라이언트"; then
    # a connected client may legitimately exist (e.g. broker CAS)
    echo "$OUT" | awk '$1 ~ /^[0-9]+$/ && NF>5 {found=1} END {exit !found}' \
      || fail "neither no-client marker nor any client row"
  fi
  echo "t4 PASS (idle filter; live-client part SKIP: csql cannot connect)"
fi
