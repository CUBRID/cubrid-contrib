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
# offsets/build-offsets.sh reusing a work tree (-w/-k) must build what the remote has now: a
# branch name resolves to origin/<branch> (the local branch of that name is never
# fast-forwarded), a tag to the tag, and a failed fetch stops the run instead of reporting a
# table of old sources as OK.  A local repository stands in for GitHub; its build.sh only
# records the commit it was run on (and fails, so nothing else runs).
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
TMP=$(mktemp -d /tmp/cub_top-bo.XXXXXX); trap 'rm -rf "$TMP"' EXIT
command -v git >/dev/null || { echo "→ SKIP (no git)"; exit 0; }
G="git -c user.name=t -c user.email=t@t -c init.defaultBranch=develop"
$G init -q "$TMP/up" && cd "$TMP/up" || exit 1
printf '#!/bin/sh\ngit rev-parse HEAD > "$HEADFILE"; exit 1\n' > build.sh; chmod +x build.sh
echo a > f; $G add -A; $G commit -qm A; A=$($G rev-parse HEAD); $G tag v1.0.0.1
cd "$TMP"; git clone -q --bare up up.git
bo(){ HEADFILE="$TMP/head" sh "$ROOT/offsets/build-offsets.sh" -r "$TMP/up.git" -w "$TMP/w" -o "$TMP/out" "$@" >"$TMP/log" 2>&1; }
bad=0; ok(){ echo "  ok   $1"; }; no(){ echo "  FAIL $1"; bad=1; }
bo develop; [ "$(cat "$TMP/head")" = "$A" ] && ok "first run (fresh clone): develop at A" || no "first run: $(cat "$TMP/head")"
cd "$TMP/up" && echo b > f && $G commit -qam B && B=$($G rev-parse HEAD) && git push -q "$TMP/up.git" develop && cd "$TMP"
bo develop; [ "$(cat "$TMP/head")" = "$B" ] && ok "reused work tree: develop is the remote's new tip B, not the stale local branch" \
                                            || no "reused work tree built $(cat "$TMP/head") (A=$A B=$B)"
grep -q "source refs/remotes/origin/develop = $B" "$TMP/log" && ok "the commit built is reported" || no "no source line"
bo v1.0.0.1; [ "$(cat "$TMP/head")" = "$A" ] && ok "a tag builds the tag" || no "tag built $(cat "$TMP/head")"
echo stale > "$TMP/head"; (cd "$TMP/w/cubrid" && git remote set-url origin "$TMP/nowhere.git")
bo develop; rc=$?
[ $rc != 0 ] && grep -q "fetch failed" "$TMP/log" && [ "$(cat "$TMP/head")" = stale ] && ok "fetch failing: stopped (exit $rc), nothing built" \
    || no "fetch failing: exit $rc, head $(cat "$TMP/head")"
if [ $bad = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
