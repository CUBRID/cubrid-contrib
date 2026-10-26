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
# The legacy build links without -lrt, so the binary must not import any function
# that lives in librt on glibc < 2.17 (clock_*, timer_*, shm_*, mq_*, aio_*, lio_listio).
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
TMP=${TMPDIR:-/tmp}/cub_top-librt.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
command -v nm >/dev/null 2>&1 || { echo "→ SKIP (nm not found)"; exit 0; }
OUT="$TMP/cub_top" sh "$DIR/build.sh" >/dev/null 2>&1 || { echo "  build failed"; echo "→ FAIL"; exit 1; }
hits=$(nm -D --undefined-only "$TMP/cub_top" | awk '{print $2}' | sed 's/@.*//' \
       | grep -E '^(clock_|timer_|shm_open|shm_unlink|mq_|aio_|lio_listio)' | sort -u)
if [ -n "$hits" ]; then
    echo "  librt-only imports (legacy build breaks on glibc < 2.17):"
    echo "$hits" | sed 's/^/    /'
    echo "→ FAIL"; exit 1
fi
echo "  ok  no librt-only imports"
echo "→ PASS"
