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
# build_fetch.sh - build cub_volmap without a local CUBRID source checkout.
#
# Fetches only the headers volmap includes (26 files) from the CUBRID repository
# and generates the two CMake-produced headers (config.h, version.h) locally.
# No clone, no cmake, no build tree.
#
#   sh tools/build_fetch.sh                 # default tag, see REF below
#   REF=v11.5.0 sh tools/build_fetch.sh     # a specific tag or commit
#   KEEP=1 sh tools/build_fetch.sh          # keep the fetched headers
#
# The headers go to a directory this run creates: WORK if it does not exist yet,
# otherwise a fresh subdirectory of it, so nothing already there is touched.  It is
# removed on exit, failures included, unless KEEP=1.
#
# Why headers at all: volmap reads on-disk structures through the engine's own
# declarations, so offsets are computed by the compiler rather than hard-coded.
# That is the point of the design - and its cost is needing these headers.
set -e

REF=${REF:-develop}
REPO=${REPO:-https://raw.githubusercontent.com/CUBRID/cubrid}
SELF=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$SELF/../.fetch}
OUT=${OUT:-$SELF/../cub_volmap}

# The exact set volmap's includes pull in (verified with g++ -MM).
HEADERS="
include/system.h
src/base/cubrid_getopt.h
src/base/databases_file.h
src/base/dynamic_array.h
src/base/environment_variable.h
src/base/error_code.h
src/base/memory_alloc.h
src/base/memory_hash.h
src/base/message_catalog.h
src/base/object_representation_constants.h
src/base/porting.h
src/base/porting_inline.hpp
src/base/release_string.h
src/base/sha1.h
src/base/util_func.h
src/compat/cache_time.h
src/compat/dbtype_def.h
src/executables/utility.h
src/storage/file_io.h
src/storage/oid.h
src/storage/storage_common.h
src/thread/thread_compat.hpp
src/transaction/log_lsa.hpp
"

# Present from 11.4 onwards only.
HEADERS_OPTIONAL="src/base/memory_cwrapper.h"

if [ -e "$WORK" ]; then
  WORK=$(mktemp -d "$WORK/build_fetch.XXXXXX")
else
  mkdir -p "$WORK"
fi
[ "${KEEP:-0}" = 1 ] || trap 'rm -rf "$WORK"' EXIT

echo "fetching headers from $REPO @ $REF"
for h in $HEADERS; do
  mkdir -p "$WORK/$(dirname "$h")"
  if ! curl -fsSL "$REPO/$REF/$h" -o "$WORK/$h"; then
    echo "failed to fetch $h" >&2
    exit 1
  fi
done

# Headers that exist only in some versions; absence is not an error.
for h in $HEADERS_OPTIONAL; do
  mkdir -p "$WORK/$(dirname "$h")"
  curl -fsSL "$REPO/$REF/$h" -o "$WORK/$h" 2>/dev/null || rm -f "$WORK/$h"
done

# config.h and version.h are produced by CMake, not stored in the repository.
# volmap needs only the feature-test macros below.  The release strings are
# recorded for reference: a version tag names the release the headers came from,
# a commit SHA or branch name does not, so those get "unknown" rather than a
# guess that would misstate which headers the binary was built against.
case "$REF" in
  v[0-9]*.[0-9]*) RV=${REF#v} ;;   # v11.4.6.1963 -> 11.4.6.1963
  *)              RV= ;;           # branch name or commit: release not determinable
esac
if [ -n "$RV" ]; then
    RV_MAJ=$(echo "$RV" | cut -d. -f1)
    RV_MIN=$(echo "$RV" | cut -d. -f2)
    RV_PAT=$(echo "$RV" | cut -d. -f3); [ -n "$RV_PAT" ] || RV_PAT=0
    RV_REL=$RV_MAJ.$RV_MIN.$RV_PAT
    echo "release $RV_REL (from REF=$REF)"
else
    RV_MAJ=0; RV_MIN=0; RV_PAT=0
    RV_REL=unknown
    RV=unknown
    echo "REF=$REF does not name a release - recording the version as unknown" >&2
fi
mkdir -p "$WORK/gen"
cat > "$WORK/gen/version.h" <<EOF
#ifndef _VERSION_H_
#define _VERSION_H_
#define MAJOR_VERSION $RV_MAJ
#define MINOR_VERSION $RV_MIN
#define PATCH_VERSION $RV_PAT
#define EXTRA_VERSION 0
#define MAJOR_RELEASE_STRING $RV_REL
#define RELEASE_STRING $RV_REL
#define BUILD_NUMBER "$RV"
#define BUILD_OS "Linux"
#define BUILD_TYPE "release"
#define PACKAGE_STRING "CUBRID $RV"
#define PRODUCT_STRING "$RV"
#define VERSION_STRING "$RV-standalone"
#endif
EOF

cat > "$WORK/gen/config.h" <<'EOF'
#ifndef _CONFIG_H_
#define _CONFIG_H_
#define HAVE_ASPRINTF 1
#define HAVE_VASPRINTF 1
#define HAVE_BASENAME 1
#define HAVE_DIRNAME 1
#define HAVE_CTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_DRAND48_R 1
#define HAVE_GETHOSTBYNAME_R 1
#define HAVE_GETHOSTBYNAME_R_GLIBC 1
#define HAVE_GETOPT_LONG 1
#define HAVE_OPEN_MEMSTREAM 1
#define HAVE_STRDUP 1
#define HAVE_ERR_H 1
#define HAVE_GETOPT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIBGEN_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_NL_TYPES_H 1
#define HAVE_REGEX_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1
#define HAVE_INT8_T 1
#define HAVE_INT16_T 1
#define HAVE_INT32_T 1
#define HAVE_INT64_T 1
#define HAVE_INTPTR_T 1
#define HAVE_UINT8_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1
#define HAVE_UINTPTR_T 1
#define SIZEOF_CHAR 1
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P 8
#define HAVE_GCC_ATOMIC_BUILTINS 1
#include "system.h"
#include "version.h"
#endif /* _CONFIG_H_ */
EOF

# file_io.h in 10.2..11.4 includes third-party compression headers (lz4, lzo).
# volmap uses none of their symbols - only the enum values declared alongside -
# so minimal stubs satisfy the include without pulling in the libraries.
mkdir -p "$WORK/stub/lzo"
cat > "$WORK/stub/lz4.h" <<'EOF'
#ifndef VOLMAP_STUB_LZ4_H
#define VOLMAP_STUB_LZ4_H
/* stub: volmap references no LZ4 symbol */
#endif
EOF
cat > "$WORK/stub/lzo/lzoconf.h" <<'EOF'
#ifndef VOLMAP_STUB_LZOCONF_H
#define VOLMAP_STUB_LZOCONF_H
/* stub: volmap calls no LZO function, but 10.2's file_io.h declares struct
   fields of these types, so the typedefs must exist.  Sizes match lzo-2.x. */
#include <stddef.h>
typedef unsigned long lzo_uint;
typedef unsigned char lzo_byte;
typedef unsigned char *lzo_bytep;
typedef void *lzo_voidp;
#endif
EOF
cat > "$WORK/stub/lzo/lzo1x.h" <<'EOF'
#ifndef VOLMAP_STUB_LZO1X_H
#define VOLMAP_STUB_LZO1X_H
#endif
EOF

# 10.2 names the TDE flag byte pflag_reserve_1; 11.0 renamed it to pflag when
# TDE was introduced.  The struct layout is identical (prv is 32 bytes in both),
# so a macro is enough - volmap reads the byte only to detect encrypted pages.
if [ -f "$WORK/src/storage/file_io.h" ] \
   && grep -q "pflag_reserve_1" "$WORK/src/storage/file_io.h" \
   && ! grep -qE "unsigned char[[:space:]]+pflag;" "$WORK/src/storage/file_io.h"; then
  echo "  (pre-11.0 header: mapping pflag -> pflag_reserve_1)"
  COMPAT="-Dpflag=pflag_reserve_1"
fi

INC="-I$SELF/../src -I$WORK/gen -I$WORK/stub -I$WORK/include \
 -I$WORK/src/base -I$WORK/src/compat -I$WORK/src/storage \
 -I$WORK/src/executables -I$WORK/src/thread -I$WORK/src/transaction"

echo "compiling"

# Compile through a log instead of a pipe: a pipeline reports grep's status, and the
# "|| true" that silences grep would swallow a compiler failure too.  The old binary
# is removed first, so a stale one cannot make the -x test below pass and have a
# failed build reported as "built".
rm -f "$OUT"
LOG="$WORK/build.log"
if g++ -x c++ -std=gnu++17 -O2 -Wall -Wextra -DNDEBUG -DVOLMAP_STANDALONE $COMPAT $INC \
     "$SELF/../src/volmap.c" "$SELF/../src/volmap_standalone.cpp" \
     -static-libstdc++ -static-libgcc -static -o "$OUT" >"$LOG" 2>&1; then
  :                             # built; warnings stay in the log
else
  echo "full-static unavailable - retrying with glibc dynamic" >&2
  rm -f "$OUT"
  if ! g++ -x c++ -std=gnu++17 -O2 -Wall -Wextra -DNDEBUG -DVOLMAP_STANDALONE $COMPAT $INC \
       "$SELF/../src/volmap.c" "$SELF/../src/volmap_standalone.cpp" \
       -static-libstdc++ -static-libgcc -o "$OUT" >"$LOG" 2>&1; then
    cat "$LOG" >&2
    echo "build failed for REF=$REF" >&2
    exit 1
  fi
fi

[ -x "$OUT" ] || { echo "compiler reported success but $OUT is missing" >&2; exit 1; }

[ "${KEEP:-0}" = 1 ] && echo "headers kept in $WORK"
echo "built: $OUT"
file "$OUT" | cut -c1-100

# Say which of the two it is: the fallback above produces a glibc-dynamic binary
# at the same path, and that one carries the build host's glibc floor.
if ldd "$OUT" 2>&1 | grep -q "not a dynamic executable"; then
  echo "  fully static - no glibc requirement"
else
  echo "  glibc-dynamic (glibc-static was unavailable): requires glibc $(objdump -T "$OUT" 2>/dev/null |
    grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1 | sed 's/GLIBC_//') or newer on the target" >&2
fi
