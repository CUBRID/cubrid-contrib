#!/usr/bin/env python3
"""Resolve a running cub_server (db name or pid) for the tests: pid, db name,
install root (from /proc/<pid>/exe) and CUBRID_DATABASES (from the server's
environment).  As a script it prints shell assignments for eval:

    eval "$(python3 tests/server_env.py demodb)"   # PID DB INSTALL DBDIR
"""
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# CUB_CTV_TOOL: a path (spaces allowed) or a command line (shell words)
_tool = os.environ.get("CUB_CTV_TOOL") or os.path.join(HERE, "..", "cub_ctv")
TOOL = [_tool] if os.path.exists(_tool) else shlex.split(_tool)


def environ_of(pid, key):
    """a variable of the server's environment, or None"""
    try:
        for kv in open(f"/proc/{pid}/environ", "rb").read().split(b"\0"):
            if kv.startswith(key.encode() + b"="):
                return kv.split(b"=", 1)[1].decode()
    except OSError:
        pass
    return None


def resolve(target):
    """dict(pid, db, install, dbdir) of the first server named by target"""
    try:
        out = subprocess.check_output(TOOL + ["-l"], universal_newlines=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, OSError):
        return None  # no cub_server at all, or no tool to ask
    for line in out.splitlines():
        f = line.split()
        if len(f) == 2 and f[0].isdigit() and target in f:
            pid = f[0]
            try:
                exe = os.readlink(f"/proc/{pid}/exe")
            except OSError:
                return None  # not readable as this user
            if exe.endswith(" (deleted)"):  # python 3.6: no removesuffix
                exe = exe[:-len(" (deleted)")]
            install = os.path.dirname(os.path.dirname(exe))
            dbdir = environ_of(pid, "CUBRID_DATABASES") or f"{install}/databases"
            return dict(pid=pid, db=f[1], install=install, dbdir=dbdir)
    return None


if __name__ == "__main__":
    s = resolve(sys.argv[1] if len(sys.argv) > 1 else "demodb")
    if s is None:
        sys.exit(f"no cub_server for {sys.argv[1] if len(sys.argv) > 1 else 'demodb'}")
    for k in ("pid", "db", "install", "dbdir"):
        print(f"{k.upper()}={shlex.quote(s[k])}")
