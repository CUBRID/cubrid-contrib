#!/bin/sh
# cub_ctv build - prefer a fully static binary so one file runs on any Linux
# distribution regardless of installed libraries.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CFLAGS="-O2 -Wall -Wextra -Wshadow"
# the sources, listed: a stray .c in src/ is not linked in.  Names without
# spaces, compiled from inside src/, so a path with spaces still builds.
SRCS="cub_ctv.c ctv_cs.c ctv_dump.c ctv_elf.c ctv_text.c ctv_field.c ctv_sched.c ctv_mem.c ctv_msg.c ctv_os.c
      ctv_pool.c ctv_proc.c ctv_screen.c ctv_verdict.c ctv_thr_probe.c ctv_thr.c ctv_tran.c ctv_frame.c ctv_rec.c
      ctv_term.c ctv_rows.c ctv_json.c ctv_sampler.c ctv_present.c ctv_live.c
      ctv_replay.c"
case "${1:-}" in
  ""|--list-sources|--dynamic) ;;
  *) echo "usage: build.sh [--dynamic | --list-sources]" >&2; exit 2 ;;
esac
# --list-sources: one full path per line (tests link the same modules)
if [ "${1:-}" = "--list-sources" ]; then
  for f in $SRCS; do printf '%s\n' "$HERE/src/$f"; done
  exit 0
fi
cd "$HERE/src"
LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT
# --dynamic: link libc dynamically (no LGPL object-file duty, see NOTICE)
if [ "${1:-}" = "--dynamic" ]; then
  gcc $CFLAGS -I. -o "$HERE/cub_ctv" $SRCS
  echo "built (dynamic): $HERE/cub_ctv"
  ! command -v file >/dev/null || file "$HERE/cub_ctv"
  exit 0
fi
# one compile: its diagnostics are shown; only a missing static libc falls
# back to a dynamic link
if LC_ALL=C gcc $CFLAGS -I. -static -o "$HERE/cub_ctv" $SRCS 2>"$LOG"; then
  cat "$LOG" >&2
  echo "built (full static): $HERE/cub_ctv"
# (GNU ld, gold and lld word it differently)
elif grep -Eq "cannot find -lc|unable to find library -lc|library not found for -lc" "$LOG"; then
  echo "full-static unavailable - building dynamic (libc only)"
  gcc $CFLAGS -I. -o "$HERE/cub_ctv" $SRCS
else
  cat "$LOG" >&2
  exit 1
fi
! command -v file >/dev/null || file "$HERE/cub_ctv"
