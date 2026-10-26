#!/bin/bash
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
# Regression: retention of I/O verdicts.
# A verdict survives its condition clearing, staying up to two rows with its timestamp,
# because an I/O anomaly is brief and would otherwise vanish as soon as it was seen.
# vd_keep_step() is driven step by step.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/verdict-keep-test.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
# The test is compiled together with cub_top.c, so it calls the dashboard's own functions.
printf '#define main cub_top_main\n#include "%s"\n#undef main\n#include "%s"\n' \
    "$ROOT/src/cub_top.c" "$ROOT/tools/verdict-keep-test.c" > "$TMP/t.c"
gcc -O2 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" 2>"$TMP/cc.err" \
  || { cat "$TMP/cc.err"; echo "-> FAIL (compile failed)"; exit 1; }
echo "verdict retention (occur, hold, recur, push down, limit)"
"$TMP/t" || { echo "→ FAIL"; exit 1; }
echo "→ PASS"
