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
# check-releases.sh - build against every current CUBRID release and report
#                     on-disk structure changes.
#
# Two things can break volmap when a release ships:
#
#   1. A header it includes changes shape.  The build catches that, so this
#      script builds the newest tag of every major.minor line from 10.2 on - the
#      10.1 tree lacks headers volmap includes (cubrid_getopt.h arrived in 10.2),
#      although 10.1 volumes are read and their structures compared below.
#   2. An on-disk structure changes in a file volmap cannot include
#      (disk_manager.c, file_manager.c/h, slotted_page.h are not standalone
#      headers, so src/storage_ondisk_layout.hpp holds a copy).  A build
#      cannot catch that - the copy still compiles and silently reads the
#      wrong offsets.  This script diffs those structures between releases
#      instead.  v11.4 inserting vol_creation into DISK_VOLUME_HEADER is the
#      case this exists for.
#
# The report has three parts, and only the second decides the exit status:
#   history       changes between consecutive releases - informational, since a
#                 change the copy already follows needs no action
#   current copy  storage_ondisk_layout.hpp against the newest release, any
#                 structure not found at all (renamed, or moved to a file this
#                 script does not fetch), and any supported release whose
#                 sources could not be fetched - these fail the check
#   develop       with --develop only - a warning, never a failure (structures
#                 and build alike)
#
# Usage:
#   sh tools/check-releases.sh              # structures + build (slow)
#   sh tools/check-releases.sh --structs    # structures only (fast, no compiler)
#   sh tools/check-releases.sh --builds     # builds only
#   sh tools/check-releases.sh --develop    # also compare against upstream develop
#
# --develop reports what the next release will bring, before it ships.  It is
# opt-in because develop moves: a change there is a warning, not a defect.
#
# Exit status: 0 clean, 1 a release build failed, 2 the copy needs updating (out
#              of date with the newest release, or a structure went missing),
#              3 both, 64 usage error.
#
# The run works only in a directory it creates: WORK if it does not exist yet,
# otherwise a fresh subdirectory of it, so nothing already there is touched.  That
# directory is removed on exit unless KEEP=1.  Logs of failed builds are kept in a
# separate directory, which the report names.
set -u

SELF=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-${TMPDIR:-/tmp}/volmap-releases.$$}
API=${API:-https://api.github.com/repos/CUBRID/cubrid/releases?per_page=100}
RAW=${RAW:-https://raw.githubusercontent.com/CUBRID/cubrid}
KEEP=${KEEP:-0}

do_structs=1; do_builds=1; with_develop=0
for a in "$@"; do
  case "$a" in
    --structs)   do_builds=0 ;;
    --builds)    do_structs=0 ;;
    --develop)   with_develop=1 ;;
    -h|--help)   sed -n '18,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *)           echo "unknown option: $a (see --help)" >&2; exit 64 ;;
  esac
done

# A WORK given by the caller may hold files of theirs, so an existing one gets a
# fresh subdirectory.  INT/TERM/HUP exit through the EXIT trap so an interrupted
# run cleans up too.
if [ -e "$WORK" ]; then
    WORK=$(mktemp -d "$WORK/volmap-releases.XXXXXX") || exit 1
else
    mkdir -p "$WORK" || exit 1
fi
cleanup () { [ "$KEEP" != "1" ] && rm -rf "$WORK"; }
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
LOGS=""                                         # created on the first failed build

# Builds need the headers volmap includes, which exist from 10.2 on (10.0 and 10.1
# lack cubrid_getopt.h).  Non-numeric refs (develop) pass.
buildable () {
    v=${1#v}; maj=${v%%.*}; rest=${v#*.}; min=${rest%%.*}
    case "$maj$min" in ''|*[!0-9]*) return 0 ;; esac
    [ "$maj" -gt 10 ] || { [ "$maj" -eq 10 ] && [ "$min" -ge 2 ]; }
}

# ---- the newest tag of each major.minor line ------------------------------
echo "querying releases"
if ! curl -fsSL "$API" -o "$WORK/releases.json"; then
    echo "cannot reach the releases API - check network access to github.com" >&2
    exit 1
fi

TAGS=$(python3 - "$WORK/releases.json" <<'PY'
import json, re, sys, collections
rel = json.load(open(sys.argv[1]))
newest = collections.OrderedDict()
for r in rel:
    t = r.get("tag_name", "")
    if not re.fullmatch(r"v\d+\.\d+(\.\d+)*", t):
        continue
    p = t.lstrip("v").split(".")
    newest.setdefault(f"{int(p[0]):03d}.{int(p[1]):03d}", t)   # releases come newest first
print(" ".join(newest[k] for k in sorted(newest)))
PY
)
[ -n "$TAGS" ] || { echo "no usable tags in the API response" >&2; exit 1; }
echo "  $(echo "$TAGS" | wc -w) release lines: $TAGS"
# develop sorts after every release, so it becomes the newest in the comparison and
# the local copy is checked against it.  Opt-in: it moves, and a change there is a
# warning about the next release rather than something to fix today.
[ "$with_develop" = "1" ] && TAGS="$TAGS develop"

rc=0

# ---- on-disk structures ---------------------------------------------------
if [ "$do_structs" = "1" ]; then
    echo
    echo "== on-disk structures =="
    for t in $TAGS; do
        mkdir -p "$WORK/$t"
        for f in storage/disk_manager.c storage/file_manager.c \
                 storage/file_manager.h storage/slotted_page.h; do
            curl -fsSL "$RAW/$t/src/$f" -o "$WORK/$t/$(basename "$f")" 2>/dev/null \
                || echo "$t $f" >>"$WORK/fetch-failed"
        done
    done

    python3 - "$WORK" "$SELF/../src/storage_ondisk_layout.hpp" $TAGS <<'PY'
import os, re, sys, difflib

work, local = sys.argv[1], sys.argv[2]
tags = sys.argv[3:]

# Structures volmap copies.  Names are the C tags, not the typedefs.
WANTED = ["disk_volume_header", "file_header", "file_extensible_data",
          "spage_header", "spage_slot", "file_heap_des", "file_btree_des"]

def structs(text):
    """struct bodies, comments and spacing normalised away."""
    out = {}
    for m in re.finditer(r'^struct\s+(\w+)\s*\{(.*?)^\};', text, re.M | re.S):
        name, body = m.group(1), m.group(2)
        if name not in WANTED:
            continue
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        body = re.sub(r'//[^\n]*', '', body)
        out[name] = [re.sub(r'\s+', ' ', l).strip()
                     for l in body.split('\n') if l.strip()]
    return out

def read(d):
    acc = {}
    if not os.path.isdir(d):
        return acc
    for fn in os.listdir(d):
        try:
            acc.update(structs(open(os.path.join(d, fn), encoding='utf-8',
                                    errors='replace').read()))
        except OSError:
            pass
    return acc

per = {t: read(os.path.join(work, t)) for t in tags}

# Files that could not be fetched, per tag.  A tag that could not be read is not
# skipped quietly: the comparison would fall back to an older release and could
# pass without ever seeing the newest format.
failed = {}
try:
    for line in open(os.path.join(work, "fetch-failed")):
        t, f = line.split(None, 1)
        failed.setdefault(t, []).append(f.strip())
except OSError:
    pass

def diff(a, b):
    for line in difflib.unified_diff(a, b, lineterm=''):
        if line.startswith(('+', '-')) and not line.startswith(('+++', '---')):
            print(f"      {line}")

def supported(tag):
    """volmap reads 10.1 and up; 10.0 is the pre-redesign format its header
    self-check rejects, and those structures do not exist there."""
    m = re.match(r'v(\d+)\.(\d+)', tag)
    return m is None or (int(m.group(1)), int(m.group(2))) >= (10, 1)

releases = [t for t in tags if per[t] and t != "develop" and supported(t)]
develop = "develop" if "develop" in tags and per.get("develop") else None

# 1. History - what changed between consecutive releases.  Informational only:
#    a change that the copy already follows (11.3 -> 11.4 vol_creation) is not
#    something to fix, so it must not fail the check.
print("  -- history (informational) --")
seen = False
chain = releases + ([develop] if develop else [])
for prev_tag, t in zip(chain, chain[1:]):
    for name in WANTED:
        a, b = per[prev_tag].get(name), per[t].get(name)
        if a is None or b is None or a == b:
            continue
        seen = True
        print(f"  CHANGED {name}: {prev_tag} -> {t}")
        diff(a, b)
if not seen:
    print("  none")

# 2. What needs action today - these fail the check.
#    A structure not found at all is the regression this script exists for:
#    renaming it, or moving it to a file that is not fetched, would otherwise make
#    every comparison a silent skip.
print("  -- current copy (fails the check) --")
fail = False
unread = False
for t in tags:
    if t == "develop" or not supported(t) or (per[t] and t not in failed):
        continue
    fail = unread = True
    print(f"  COULD NOT READ {t}: " + (", ".join(failed.get(t, [])) or "no structures found"))
for t in releases:
    if not supported(t):
        continue
    gone = [n for n in WANTED if n not in per[t]]
    if gone:
        fail = True
        print(f"  NOT FOUND in {t}: " + ", ".join(gone))
mine = structs(open(local, encoding='utf-8', errors='replace').read())
gone = [n for n in WANTED if n not in mine]
if gone:
    fail = True
    print("  NOT FOUND in storage_ondisk_layout.hpp: " + ", ".join(gone))
if releases:
    newest = releases[-1]
    for name in WANTED:
        a, b = mine.get(name), per[newest].get(name)
        if a is None or b is None or a == b:
            continue
        fail = True
        print(f"  COPY OUT OF DATE {name}: storage_ondisk_layout.hpp vs {newest}")
        diff(a, b)
if unread:
    print("  -> a source could not be fetched (network, or the file moved in that"
          " release): fix the file list above, then re-run")
if fail and not unread:
    print("  -> update src/storage_ondisk_layout.hpp and docs/ondisk-format.md")
if not fail:
    print(f"  storage_ondisk_layout.hpp matches {releases[-1] if releases else '?'}")

# 3. develop - the next release, before it ships.  A warning, never a failure:
#    develop moves, and nothing in it is released yet.
if "develop" in tags and (not develop or "develop" in failed):
    print("  -- develop (warning only) --")
    print("  COULD NOT READ develop: " + (", ".join(failed.get("develop", [])) or "no structures found"))
if develop:
    print("  -- develop (warning only) --")
    warn = False
    gone = [n for n in WANTED if n not in per[develop]]
    if gone:
        warn = True
        print("  NOT FOUND in develop: " + ", ".join(gone))
    for name in WANTED:
        a, b = mine.get(name), per[develop].get(name)
        if a is None or b is None or a == b:
            continue
        warn = True
        print(f"  NEXT RELEASE {name}: storage_ondisk_layout.hpp vs develop")
        diff(a, b)
    if not warn:
        print("  none")

sys.exit(2 if fail else 0)
PY
    # Any non-zero status counts, not just 2: an exception exits 1, and swallowing
    # that would report a clean run on a check that never finished.
    case $? in
      0) ;;
      2) rc=$((rc + 2)) ;;
      *) echo "  the structure check did not finish" >&2; rc=$((rc + 2)) ;;
    esac
fi

# ---- builds ---------------------------------------------------------------
if [ "$do_builds" = "1" ]; then
    echo
    echo "== builds =="
    failed=""
    for t in $TAGS; do
        printf '  %-18s ' "$t"
        if ! buildable "$t"; then
            echo "skipped (no volmap build before 10.2)"
            continue
        fi
        # Set OUT from the outer WORK first: in a prefix assignment list a later
        # word already sees the WORK assigned before it.
        out="$WORK/cub_volmap-$t"
        if REF="$t" WORK="$WORK/build-$t" OUT="$out" \
           sh "$SELF/build_fetch.sh" >"$WORK/$t.log" 2>&1; then
            echo "OK"
        else
            # Keep only the log; the downloads and binaries go with the work directory.
            [ -n "$LOGS" ] || LOGS=$(mktemp -d "${TMPDIR:-/tmp}/volmap-build-logs.XXXXXX")
            cp "$WORK/$t.log" "$LOGS/$t.log"
            # develop is the next release: its build failing is a warning, not a failure
            if [ "$t" = "develop" ]; then
                echo "FAILED  (warning only - see $LOGS/$t.log)"
            else
                echo "FAILED  (see $LOGS/$t.log)"
                failed="$failed $t"
            fi
        fi
    done
    [ -n "$failed" ] && { echo "  failed:$failed"; rc=$((rc + 1)); }
fi

exit $rc
