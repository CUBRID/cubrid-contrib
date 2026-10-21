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
# build_standalone.sh - build the standalone volmap binary, copyable to any server as a single file
set -e
SRC=${SRC:?set SRC=/path/to/cubrid-src (CUBRID source checkout)}
BUILD=${BUILD:-$SRC/build_release}
OUT=${OUT:-$(dirname "$0")/../cub_volmap}

SELF=$(cd "$(dirname "$0")" && pwd)
INC="-I$SRC/src/executables -I$SELF/../src -I$SRC/src/storage -I$SRC/src/base -I$SRC/src/compat \
 -I$SRC/src/transaction -I$SRC/src/object -I$SRC/src/parser -I$SRC/src/query \
 -I$SRC/src/connection -I$SRC/src/communication -I$SRC/src/thread -I$SRC/src/monitor \
 -I$SRC/src/session -I$SRC/src/xasl -I$SRC/src/api -I$SRC/src/sp -I$SRC/src/method \
 -I$BUILD -I$SRC/include -I$SRC/3rdparty/rapidjson/include"

# Compile through a log rather than a pipe: a pipeline reports grep's status, so the
# "|| true" that silences grep would hide a compiler failure as well.  The old binary
# is removed first so a stale one cannot satisfy the -x test and have a failed build
# reported as "built".
LOG=$(mktemp "${TMPDIR:-/tmp}/volmap-build.XXXXXX")
trap 'rm -f "$LOG"' EXIT
rm -f "$OUT"
if g++ -x c++ -std=gnu++17 -O2 -Wall -Wextra -DNDEBUG -DVOLMAP_STANDALONE $INC \
     "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
     -static-libstdc++ -static-libgcc -static -o "$OUT" >"$LOG" 2>&1; then
  :                             # built; warnings stay in the log
else
  echo "full-static unavailable - building with static libstdc++/libgcc (glibc dynamic)"
  rm -f "$OUT"
  if ! g++ -x c++ -std=gnu++17 -O2 -Wall -Wextra -DNDEBUG -DVOLMAP_STANDALONE $INC \
       "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
       -static-libstdc++ -static-libgcc -o "$OUT" >"$LOG" 2>&1; then
    cat "$LOG" >&2
    echo "build failed: $OUT" >&2
    exit 1
  fi
fi
[ -x "$OUT" ] || { echo "compiler reported success but $OUT is missing" >&2; exit 1; }
echo "built: $OUT"; file "$OUT" | cut -c1-100; ldd "$OUT" 2>&1 | head -3

# Say which of the two it is: the fallback above produces a glibc-dynamic binary
# at the same path, and that one carries the build host's glibc floor.
if ldd "$OUT" 2>&1 | grep -q "not a dynamic executable"; then
  echo "  fully static - no glibc requirement"
else
  echo "  glibc-dynamic (glibc-static was unavailable): requires glibc $(objdump -T "$OUT" 2>/dev/null |
    grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1 | sed 's/GLIBC_//') or newer on the target" >&2
fi

