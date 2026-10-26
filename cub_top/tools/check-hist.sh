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
# Compare history (what the plots read) against terse (the tree and dashboard's source)
# at the same instant: the time-series samples must use the same classification results
# as the tree, so dyn-heap, heap allocation and server agree across views.
# One run yields both stdout (terse) and stderr (hist), so the instant is identical.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${CUBMEM_BIN:-"$ROOT/cub_top"}
[ -x "$BIN" ] || { echo "SKIP: no binary ($BIN) - run build.sh first"; exit 0; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
"$BIN" -d --dump-hist >"$TMP/terse" 2>"$TMP/hist" || true

grep -q '^server.up=1' "$TMP/terse" || { echo "SKIP: cub_server not running"; exit 0; }
[ -s "$TMP/hist" ] || { echo "FAIL: no --dump-hist output"; exit 1; }

g(){ tr ' ' '\n' <"$1" | sed -n "s/^$2=//p" | head -1; }
fail=0
# hist key | terse key | scale (multiply terse to get bytes) | tolerance
chk(){
  h=$(g "$TMP/hist" "$1"); t=$(g "$TMP/terse" "$2")
  [ -n "$h" ] || { echo "FAIL: hist.$1 missing"; fail=1; return; }
  if [ -z "$t" ]; then t=0; fi
  r=$(awk -v h="$h" -v t="$t" -v m="$3" -v e="$4" \
      'BEGIN{v=t*m; d=h-v; if(d<0)d=-d; print (d<=e)?"ok":"BAD "h" vs "v}')
  case "$r" in ok) echo "  ok  $1 == $2";; *) echo "FAIL $1: $r"; fail=1;; esac
}
echo "same-instant comparison (history vs terse)"
chk hist.rss     server.rss_kb              1024 0
chk hist.srv_rss server.rss_kb              1024 0
chk hist.vsz     server.mapped_kb           1024 0
chk hist.db      region.data_buffer.rss_bytes   1 0
chk hist.lg      region.log_buffer.rss_bytes    1 0
chk hist.dyn     region.dynamic_heap.rss_bytes  1 0
chk hist.dynmap  region.dynamic_heap.mapped_bytes 1 0
chk hist.cub     total.cubrid_rss_bytes         1 0
chk hist.minflt  server.minflt_per_s            1 0
chk hist.majflt  server.majflt_per_s            1 0
chk hist.devutil hostdev.util_pct                  1 0.05
# Tier RSS (the source of the process box's delta and sparkline columns) == terse tier.*
chk hist.rss_srv    tier.server_rss_bytes   1 0
chk hist.rss_master tier.master_rss_bytes   1 0
chk hist.rss_broker tier.broker_rss_bytes   1 0
chk hist.rss_cas    tier.cas_rss_bytes      1 0
chk hist.rss_pl     tier.pl_rss_bytes       1 0
# BCB direct read: what the B/D panels and the verdict read must share its source with terse pgbuf.*/log.*
if grep -q '^pgbuf.enabled=1' "$TMP/terse"; then
  chk hist.pg_dirty pgbuf.dirty                      1 0
  chk hist.pg_lag   log.flush_lag_pages              1 0
  chk hist.pg_span  log.dirty_span_pages             1 0
  awk 'BEGIN{FS="[= ]"} {for(i=1;i<NF;i++){if($i=="pgbuf.resident")r=$(i+1); if($i=="pgbuf.num_buffers")n=$(i+1)}} END{if(n+0>0){printf "%.2f\n",100*r/n}else{print 0}}' "$TMP/terse" > "$TMP/res"
  hr=$(g "$TMP/hist" hist.pg_res_pct); tr2=$(cat "$TMP/res")
  awk -v h="$hr" -v t="$tr2" 'BEGIN{d=h-t; if(d<0)d=-d; exit (d<=0.05)?0:1}' && echo "  ok  hist.pg_res_pct == pgbuf.resident/num_buffers" || { echo "FAIL hist.pg_res_pct $hr vs $tr2"; fail=1; }
else echo "  skip pgbuf (disabled)"; fi

# Identity: db + lg + dyn + etc == srv_rss (a PSS denominator breaks it here)
awk 'BEGIN{FS="[= ]"} {for(i=1;i<NF;i++){if($i=="hist.db")db=$(i+1);
  if($i=="hist.lg")lg=$(i+1); if($i=="hist.dyn")dy=$(i+1);
  if($i=="hist.etc")et=$(i+1); if($i=="hist.srv_rss")sv=$(i+1)}}
END{s=db+lg+dy+et; d=s-sv; if(d<0)d=-d;
  if(d<=1){print "  ok  identity db+lg+dyn+etc == srv_rss"}
  else{print "FAIL identity: "s" vs "sv; exit 1}}' "$TMP/hist" || fail=1

[ "$fail" = 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
