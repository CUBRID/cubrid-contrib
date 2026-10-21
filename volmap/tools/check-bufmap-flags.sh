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
# A --bufmap page is "not yet on the volume" when its BCB has DIRTY (bit31) or
# FLUSHING (bit30) set: the engine clears DIRTY when a flush starts and keeps FLUSHING
# until the write is done.  A snapshot is written here with one page of each kind -
# dirty, flushing, both, clean - and the JSON must count 3 unflushed and 2 flushing.
#
#   sh tools/check-bufmap-flags.sh [db]      (default demodb; the database need not run)
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
BIN=${VOLMAP:-$ROOT/cub_volmap}
DB=${1:-demodb}
TMP=$(mktemp -d /tmp/volmap-bufflags.XXXXXX); trap 'rm -rf "$TMP"' EXIT
[ -x "$BIN" ] || { echo "→ FAIL (no binary: $BIN)"; exit 1; }
# four allocated pages of volume 0, from the map itself
"$BIN" --format=json "$DB" > "$TMP/base.json" 2>/dev/null || { echo "→ SKIP ($DB not readable)"; exit 0; }
python3 - "$BIN" "$DB" "$TMP" <<'PY' || exit 1
import json, struct, subprocess, sys
binp, db, tmp = sys.argv[1:4]
base = json.load(open(tmp + "/base.json"))
# the snapshot names the database; volmap compares that to the name it derives from its
# argument (a vinf path loses its directory and the _vinf suffix)
label = db.rsplit("/", 1)[-1]
if label.endswith("_vinf"): label = label[:-5]
# pick 4 page ids that exist: the file headers of 4 files on volume 0
pages = [int(f["vfid"].split("|")[1]) for f in base["files"] if f["vfid"].startswith("0|")][:4]
if len(pages) < 4:
    print("→ SKIP (fewer than 4 files on volume 0)"); sys.exit(0)
DIRTY, FLUSHING = 0x80000000, 0x40000000
flags = [DIRTY, FLUSHING, DIRTY | FLUSHING, 0]
recs = b"".join(struct.pack("<hhiIiQQ", 0, 1, p, fl, 1, 0, 0) for p, fl in zip(pages, flags))
hdr = struct.pack("<8sIIqqiiiiQQQQ32s", b"CBCBMAP1", 1, 32, 0, 0, 100, 4, 3, 16384,
                  0, 0, 0, (1 << 64) - 1, label.encode())
open(tmp + "/snap.bcb", "wb").write(hdr + recs)
r = subprocess.run([binp, "--format=json", "-B", tmp + "/snap.bcb", db], capture_output=True, text=True)
if not r.stdout.strip():
    print("  FAIL   volmap gave no JSON with the snapshot: %s" % r.stderr.strip()[:200]); sys.exit(1)
d = json.loads(r.stdout)
v0 = [v for v in d["volumes"] if v["volid"] == 0][0]
ok = v0["buffered_pages"] == 4 and v0["unflushed_pages"] == 3 and v0["flushing_pages"] == 2
print("  %s   snapshot with dirty / flushing / both / clean: buffered %d, unflushed %d (want 3), flushing %d (want 2)"
      % ("ok  " if ok else "FAIL", v0["buffered_pages"], v0["unflushed_pages"], v0["flushing_pages"]))
sys.exit(0 if ok else 1)
PY
echo "→ PASS"
