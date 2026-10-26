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
# check-releases.sh - report which offset tables have fallen behind the
#                     current CUBRID releases.
#
# Method B reads the server's structures through offsets/cubrid-<version>.tbl.
# Those offsets are whatever the compiler chose, so they belong to one build,
# and a table made for 11.3.3 is not guaranteed to fit 11.3.5.  cub_top falls
# back to DWARF extraction or anchor probing when no table matches, so a stale
# set degrades quietly rather than failing - which is why this is a separate
# check rather than something a test would catch.
#
# This script only reports.  Producing a table means building that release in
# debug mode, which takes tens of minutes, so the build stays a deliberate
# step: offsets/build-offsets.sh <tag>.
#
# Usage:
#   sh tools/check-releases.sh
#
# Exit status: 0 every release line has a table for its newest patch,
#              2 at least one is missing or behind.
set -u

SELF=$(cd "$(dirname "$0")" && pwd)
OFFS=${OFFS:-$SELF/../offsets}
API=${API:-https://api.github.com/repos/CUBRID/cubrid/releases?per_page=100}
TMP=${TMPDIR:-/tmp}/cub_top-releases.$$

trap 'rm -f "$TMP"' EXIT

echo "querying releases"
if ! curl -fsSL "$API" -o "$TMP"; then
    echo "cannot reach the releases API - check network access to github.com" >&2
    exit 1
fi

python3 - "$TMP" "$OFFS" <<'PY'
import json, os, re, sys, collections

rel = json.load(open(sys.argv[1]))
offs = sys.argv[2]

# newest tag of each major.minor line; the API returns newest first
newest = collections.OrderedDict()
for r in rel:
    t = r.get("tag_name", "")
    if not re.fullmatch(r"v\d+\.\d+(\.\d+)*", t):
        continue
    p = t.lstrip("v").split(".")
    newest.setdefault((int(p[0]), int(p[1])), t)

have = sorted(f[7:-4] for f in os.listdir(offs) if f.startswith("cubrid-")
              and f.endswith(".tbl"))

def parts(v):
    return [int(x) for x in v.split(".")]

stale = []
print("\n  %-8s %-16s %s" % ("line", "newest release", "table"))
for (maj, minr), tag in sorted(newest.items()):
    ver = tag.lstrip("v")
    mine = [h for h in have if parts(h)[:2] == [maj, minr]]
    if not mine:
        note, bad = "none", True
    else:
        best = max(mine, key=parts)
        bad = parts(best) < parts(ver)
        note = best + ("  (behind)" if bad else "")
    if bad:
        stale.append((f"{maj}.{minr}", ver))
    print("  %-8s %-16s %s" % (f"{maj}.{minr}", ver, note))

extra = [h for h in have if (parts(h)[0], parts(h)[1]) not in newest]
if extra:
    print("\n  tables for lines with no current release: " + ", ".join(extra))

if not stale:
    print("\n  every release line has a table for its newest patch")
    sys.exit(0)

print("\n  behind or missing: " + ", ".join(f"{l} (newest {v})" for l, v in stale))
print("""
  A table comes from the DWARF in libcubrid.so, so the published build is
  enough - no compiler needed.  Take the library out of the tarball without
  unpacking the rest:

    curl -fsSL https://ftp.cubrid.org/CUBRID_Engine/<line>/ |
      grep -oE 'CUBRID-[0-9.]+-[0-9a-f]+-Linux\\.x86_64\\.tar\\.gz'
    curl -fsSL <that url> | tar -xz --wildcards '*/lib/libcubrid.so*'
    sh tools/collect-offsets.sh -o offsets ./CUBRID
    sh tools/build.sh            # embeds the tables

  ftp.cubrid.org can lag the GitHub releases by a patch, and not every line
  is published there; offsets/build-offsets.sh builds from source when no
  published binary exists, at tens of minutes per version.""")
sys.exit(2)
PY
