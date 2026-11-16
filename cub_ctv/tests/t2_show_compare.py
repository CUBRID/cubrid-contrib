#!/usr/bin/env python3
"""T2: cub_ctv snapshot vs SHOW CRITICAL SECTIONS reference.
Runs cub_ctv (before), SHOW via csql, cub_ctv (after); every SHOW nenter and
nwait must lie in [before, after] (counters are monotonic; the SHOW connection
itself moves some of them). Also T3: two /proc reads with no SQL in between
must be identical except for counters moved by the server's own background
activity (only LOG, LOG_ARCHIVE and TRAN_TABLE on an idle db), and so must the
waiting counts (waitR/waitW)."""
import json
import os
import re
import subprocess
import sys
import tempfile

DB = sys.argv[1] if len(sys.argv) > 1 else "demodb"
HERE = os.path.dirname(os.path.abspath(__file__))


sys.path.insert(0, HERE)
import server_env  # noqa: E402

TOOL = server_env.TOOL  # the same tool resolution as every other test

_srv = server_env.resolve(DB)
if _srv is None:
    sys.exit(f"T2: no cub_server for {DB}")
PID, DB, DBDIR = _srv["pid"], _srv["db"], _srv["dbdir"]
CUB = os.environ.get("CUB") or _srv["install"]


def cub_ctv():
    d = json.loads(subprocess.check_output(TOOL + [PID, "--json"], universal_newlines=True))
    rows = {c["name"]: dict(wr=c["waitR"], ww=c["waitW"], nenter=c["nenter"],
                            nwait=c["nwait"]) for c in d["cs"] if "nenter" in c}
    assert len(rows) == 18, f"cub_ctv parse: {len(rows)}"
    return rows


def show_cs():
    with tempfile.TemporaryDirectory() as td:  # csql writes csql.err into the cwd
        out = subprocess.check_output(
            [f"{CUB}/bin/csql", "-u", "dba", "-C", DB, "-c", "SHOW CRITICAL SECTIONS;"],
            universal_newlines=True, cwd=td,
            env={"CUBRID": CUB, "CUBRID_DATABASES": DBDIR,
                 "LD_LIBRARY_PATH": f"{CUB}/lib", "PATH": "/usr/bin:/bin"})
    rows = {}
    for line in out.splitlines():
        mm = re.match(r"\s*(\d+)\s+'([^']+)'\s+'[^']*'\s+(\d+)\s+(\d+)\s+\S+\s+\S+\s+(\d+)\s+(\d+)", line)
        if mm:
            rows[mm.group(2)] = dict(wr=int(mm.group(3)), ww=int(mm.group(4)),
                                     nenter=int(mm.group(5)), nwait=int(mm.group(6)))
    assert len(rows) == 18, f"SHOW parse: {len(rows)}"
    return rows


before = cub_ctv()
mid = cub_ctv()          # T3 pair: two pure /proc reads back to back
orc = show_cs()
after = cub_ctv()

# T3: our own reads must not move any counter (background LOG* excepted)
t3_bad = [n for n in before
          if n not in ("LOG", "LOG_ARCHIVE", "TRAN_TABLE")
          and any(before[n][k] != mid[n][k] for k in ("nenter", "nwait", "wr", "ww"))]
print("T3 self-pollution:", "FAIL " + str(t3_bad) if t3_bad else "PASS (0 counters moved by cub_ctv reads)")

# T2: SHOW value within [before, after]
fails = []
def within(b, o, a):
    """b <= o <= a on u32 counters that may wrap between the reads"""
    return (o - b) & 0xFFFFFFFF <= (a - b) & 0xFFFFFFFF


for n, o in orc.items():
    for k in ("nenter", "nwait"):
        if not within(before[n][k], o[k], after[n][k]):
            fails.append((n, k, before[n][k], o[k], after[n][k]))
print("T2 SHOW bracket:", "FAIL " + str(fails) if fails else
      f"PASS (18 CS x nenter/nwait all within [before, after])")
moved = {n: orc[n]["nenter"] - before[n]["nenter"] for n in orc
         if orc[n]["nenter"] != before[n]["nenter"]}
print("   counters moved by the SHOW connection itself:", moved)
sys.exit(1 if fails or t3_bad else 0)
