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
# L3 - check the source's offset constants against the committed golden table.
# This catches an offset that is aligned but wrong, which the runtime cannot distinguish.
set -e
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
# Source location: src/.
SRC="$ROOT/src/cub_top.c"
GOLD=${1:-$DIR/rules-11.5.h}
fail=0
val_gold(){ sed -n "s/^#define $1[[:space:]]\+\([0-9-]\+\).*/\1/p" "$GOLD"; }
val_c(){    sed -n "s/^#define $1[[:space:]]\+\([0-9-]\+\).*/\1/p" "$SRC" | head -1; }
# The Python implementation is frozen; only the C source is compared

cmp3(){ # key  c_macro  (the third argument is a legacy py key and is ignored)
  g=$(val_gold "$2"); c=$(val_c "$2")
  [ -n "$g" ] || { echo "  ! $2 missing from the golden table"; fail=1; return; }
  st="match"
  [ "$c" = "$g" ] || { st="differ(C=$c)"; fail=1; }
  printf "  %-22s golden %-5s C %-5s %s\n" "$2" "$g" "${c:--}" "$st"
}
echo "offset L3 comparison (golden: $(basename "$GOLD"))"
cmp3 x OFF_XCACHE_ENTRIES  xcache.entry_count
cmp3 x OFF_XCACHE_USAGE    xcache.usage_cache
cmp3 x OFF_XCACHE_CLONE    xcache.usage_clone
cmp3 x OFF_QLIST_NPAGES    qlist.n_pages
cmp3 x OFF_QPOOL_NENT      qpool.n_entries
cmp3 x OFF_LF_ALLOCCNT     lf.alloc_cnt
for k in SZ_SESSION_STATE SZ_CATALOG_ENTRY SZ_FPCACHE_ENT SZ_LK_TRAN_LOCK SZ_CSS_CONN_ENTRY SZ_VACUUM_DATA; do
  g=$(val_gold $k); c=$(val_c $k)
  if [ "$c" = "$g" ]; then printf "  %-22s golden %-5s C %-5s %s\n" $k "$g" "$c" "match"
  else printf "  %-22s golden %-5s C %-5s %s\n" $k "$g" "${c:--}" "differ"; fail=1; fi
done
[ $fail -eq 0 ] && echo "→ PASS" || { echo "-> FAIL (regenerate the golden table: tools/dwoff <libcubrid.so> --emit-c)"; exit 1; }
