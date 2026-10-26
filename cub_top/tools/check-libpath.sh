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
# The library path in /proc/<pid>/maps is the rest of the line: an install path with spaces
# must be read whole, and a trailing " (deleted)" noticed.  Without map_files (a 3.10 kernel
# opens it for root only) the disk file is read only while it still is the running inode: a
# replaced library would hand over another build's symbols, so method B is refused instead.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d /tmp/cub_top-libpath.XXXXXX); trap 'rm -rf "$TMP"' EXIT
D="$TMP/CUBRID 11.5 install/lib"; mkdir -p "$D"
echo 'int x=1;' > "$TMP/l.c"; gcc -shared -fPIC -o "$D/libcubrid.so.11.5" "$TMP/l.c" || { echo "→ FAIL (build)"; exit 1; }
cp "$D/libcubrid.so.11.5" "$TMP/other.so"
cat > "$TMP/srv.c" <<'C'
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int c,char**v){ int fd=open(v[1],O_RDONLY); void*p=mmap(0,4096,PROT_READ,MAP_PRIVATE,fd,0); (void)p; pause(); return 0; }
C
gcc -O1 -o "$TMP/srv" "$TMP/srv.c"
mk(){ # name sed-expr: a test binary from the source with an optional edit
    sed -e "$2" "$SRC" > "$TMP/$1.c"
    printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/$1.c" > "$TMP/$1.t.c"
    cat >> "$TMP/$1.t.c" <<'C'
int main(int c,char**v){ char so[4096]; unsigned long b=lib_base(atoi(v[1]),"libcubrid.so.",so,sizeof so);
    printf("%lx|%s|%s\n",b,so,g_lib_why); return 0; }
C
    gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/$1" "$TMP/$1.t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
}
mk real 's|^x^x^|&|'
mk nomf 's|"/proc/%d/map_files/%lx-%lx"|"/nonexistent/%d/map_files/%lx-%lx"|'
grep -q nonexistent "$TMP/nomf.c" || { echo "→ FAIL (map_files path not found in the source)"; exit 1; }
"$TMP/srv" "$D/libcubrid.so.11.5" & s=$!; sleep 0.3
bad=0; ok(){ echo "  ok   $1"; }; no(){ echo "  FAIL $1"; bad=1; }
r=$("$TMP/real" $s); case "$r" in *"|/proc/$s/map_files/"*) ok "map_files used when it opens";; *) no "map_files: $r";; esac
r=$("$TMP/nomf" $s); case "$r" in *"|$D/libcubrid.so.11.5|") ok "no map_files: the path with spaces read whole (\"$(echo "$r" | cut -d'|' -f2 | sed "s|$TMP/||")\")";; *) no "path with spaces: $r";; esac
mv "$TMP/other.so" "$D/libcubrid.so.11.5"     # replaced on disk: the running inode is now "(deleted)"
grep -q "libcubrid.so.11.5 (deleted)" /proc/$s/maps && ok "maps shows the replaced library as (deleted)" || no "no (deleted) in maps"
r=$("$TMP/real" $s); case "$r" in *"|/proc/$s/map_files/"*) ok "replaced, map_files opens: the running bytes are read";; *) no "replaced with map_files: $r";; esac
r=$("$TMP/nomf" $s); case "$r" in *"||"*replaced*|*"||"*교체*) ok "replaced, no map_files: refused - \"$(echo "$r" | cut -d'|' -f3)\"";; *) no "replaced without map_files not refused: $r";; esac
kill $s 2>/dev/null; wait $s 2>/dev/null
if [ $bad = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
