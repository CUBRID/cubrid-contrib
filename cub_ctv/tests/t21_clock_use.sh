#!/bin/bash
# t21: intervals, periods and retry schedules use the monotonic clock
# (mono_s), so a step of the wall clock (NTP, date -s, a VM restore) neither
# stalls nor bunches them.  The wall clock (time) is only for the moments
# shown or recorded: the frame time (view.ts) and the record file name.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../src"
ALLOWED='^(ctv_frame\.c:.*view\.ts = \(long\) time \(NULL\);|ctv_live\.c:.*time_t t = time \(NULL\);)'
BAD=$(cd "$SRC" && grep -n -E '\btime \(NULL\)|\btime\(NULL\)|\btime \(0\)|gettimeofday|CLOCK_REALTIME' *.c \
  | sed -E 's/^([^:]+):[0-9]+:/\1:/' | grep -v -E "$ALLOWED")
if [ -n "$BAD" ]; then
  echo "t21 FAIL: the wall clock used outside the shown/recorded moments:"
  echo "$BAD"
  exit 1
fi
N=$(cd "$SRC" && grep -c -E '\btime \(NULL\)' *.c | awk -F: '{s+=$2} END {print s}')
[ "$N" -eq 3 ] || { echo "t21 FAIL: expected 3 wall-clock reads (2 view.ts, 1 record name), found $N"; exit 1; }
echo "t21 PASS: intervals and schedules on the monotonic clock; the wall clock only for view.ts and the record name"
