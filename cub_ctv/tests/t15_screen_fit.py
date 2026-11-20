#!/usr/bin/env python3
"""t15: the summary screen fits the terminal (no server).
Replays the live fixture on pseudo-terminals of several sizes, in English and
Korean.  Every frame must take at most rows - 1 screen rows (a wrapped line
counts as several), so the screen never scrolls; from 80 columns up no line
may be wider than the terminal; a wide terminal keeps the "guards" column."""
import os, pty, fcntl, termios, struct, time, select, re, sys, unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get("CUB_CTV_TOOL", os.path.join(HERE, "..", "cub_ctv"))
REC = os.path.join(HERE, "fixtures", "demodb-live.rec")
ESC = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

def cols(s):
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)

def frames(c, r, lang, sec=2.5):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.pop("COLUMNS", None)
        os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
        os.execv(TOOL, ["cub_ctv", "--replay", REC, "--replay-speed", "4", "--" + lang])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, 0, 0))
    buf, t = b"", time.time()
    while time.time() - t < sec:
        if select.select([fd], [], [], 0.05)[0]:
            try:
                d = os.read(fd, 1 << 16)
            except OSError:
                break
            if not d:
                break
            buf += d
    try:
        os.kill(pid, 15)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    txt = buf.decode("utf-8", "replace")
    # whole frames only: the last one may be cut by the kill
    out = [f.split("\x1b[?1049l")[0] for f in txt.split("\x1b[H\x1b[2J")[1:-1]]
    return [ESC.sub("", f).replace("\r", "").rstrip("\n").split("\n") for f in out]

def fail(m):
    print("t15 FAIL:", m)
    sys.exit(1)

n = 0
for c, r in ((80, 24), (80, 40), (100, 30), (120, 30), (200, 50), (60, 20)):
    for lang in ("en", "ko"):
        fs = frames(c, r, lang)
        if not fs:
            fail("%dx%d %s: no frame" % (c, r, lang))
        for f in fs:
            rows = sum(max(1, -(-cols(l) // c)) for l in f)
            if rows > r - 1:
                fail("%dx%d %s: %d screen rows" % (c, r, lang, rows))
            wide = [l for l in f if cols(l) > c]
            if c >= 80 and wide:
                fail("%dx%d %s: line wider than the terminal: %r" % (c, r, lang, wide[0]))
            if c >= 200 and not any(("guards" if lang == "en" else "보호 대상") in l for l in f):
                fail("%dx%d %s: guards column missing" % (c, r, lang))
            n += 1
print("t15 PASS: %d frames fit 6 terminal sizes (en/ko), none scrolls" % n)
