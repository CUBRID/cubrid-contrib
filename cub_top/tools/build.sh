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
# cub_top build script - no external library dependencies
#   ./build.sh            dynamic build for this system (most compatible)
#   ./build.sh static     static standalone binary (subject to the build glibc's kernel floor)
#   ./build.sh legacy     old systems (CentOS 6 and the like) - run it on the target itself
set -e
CC=${CC:-gcc}
SRC=$(dirname "$0")/../src/cub_top.c
# Embedded offset tables: regenerate when a .tbl is newer than the header, so adding a
# table to offsets/ takes effect on the next build without a separate step.
HDR=$(dirname "$0")/../src/offsets_embedded.h
GEN=$(dirname "$0")/../offsets/gen-embedded.sh
if [ -f "$GEN" ]; then
    need=0
    [ -f "$HDR" ] || need=1
    for t in "$(dirname "$0")"/../offsets/cubrid-*.tbl; do
        [ -f "$t" ] || continue
        [ "$t" -nt "$HDR" ] && need=1
    done
    [ $need -eq 1 ] && sh "$GEN" >/dev/null
fi
OUT=${OUT:-$(dirname "$0")/../cub_top}
case "${1:-dynamic}" in
  static)
    # A statically linked glibc runs only on kernels at or above the version that glibc requires.
    #       Check the floor with NT_GNU_ABI_TAG from readelf -n <binary>.
    $CC -O2 -std=gnu99 -static -o "$OUT" "$SRC"
    echo "static build complete: $OUT"
    readelf -n "$OUT" 2>/dev/null | grep -A1 'ABI version' | tail -1 | sed 's/^/  minimum kernel:/' || true
    ;;
  legacy)
    # Old environments such as CentOS 6 / RHEL 6 (gcc 4.4, glibc 2.12, kernel 2.6.32).
    # -lrt is needed only for clock_* on glibc < 2.17; this source uses gettimeofday, so it is not.
    $CC -O2 -std=gnu99 -o "$OUT" "$SRC"
    echo "legacy build complete: $OUT"
    ;;
  *)
    $CC -O2 -std=gnu99 -Wall -o "$OUT" "$SRC"
    echo "build complete: $OUT"
    ;;
esac
