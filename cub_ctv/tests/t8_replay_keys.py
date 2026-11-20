#!/usr/bin/env python3
"""t8: interactive replay keys on a pseudo-terminal (no server).
Drives --replay on the live fixture: pause, step, speed and quit must change
the REPLAY footer as documented and leave the alternate screen on exit."""
import os, pty, fcntl, termios, struct, time, select, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get("CUB_CTV_TOOL", os.path.join(HERE, "..", "cub_ctv"))
REC = os.path.join(HERE, "fixtures", "demodb-live.rec")

def run(keys, tail=0.8, rec=REC):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(LANG="en_US.UTF-8", TERM="xterm", COLUMNS="140")
        os.execv(TOOL, ["cub_ctv", "--replay", rec])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 140, 0, 0))
    buf = b""
    def pump(sec):
        nonlocal buf
        t = time.time()
        while time.time() - t < sec:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(fd, 1 << 16)
                except OSError:
                    return
                if not d:
                    return
                buf += d
    for k, delay in keys:
        pump(delay)
        os.write(fd, k)
    pump(tail)
    try:
        os.kill(pid, 15)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    txt = buf.decode("utf-8", "replace")
    return [(int(a), float(x), bool(p)) for a, x, p in
            re.findall(r"REPLAY (\d+)/\d+  x([0-9.]+)(  PAUSED)?", txt)], txt

def fail(m):
    print("t8 FAIL:", m); sys.exit(1)

# pause: the frame number stops advancing
f, _ = run([(b" ", 2.4), (b"", 1.5)])
if not f or not f[-1][2] or f[-1][0] != f[-2][0]:
    fail("space did not pause (%r)" % f[-3:])
# step: left/right and , . move one window while paused; clamped at 1
f, _ = run([(b" ", 1.3), (b"\x1b[D", 0.2), (b"\x1b[D", 0.2), (b"\x1b[C", 0.2), (b".", 0.2), (b",", 0.2)])
seq = [n for n, _, p in f if p]
if seq[-5:] != [1, 1, 2, 3, 2] and seq[-4:] != [1, 2, 3, 2]:
    fail("step sequence %r" % seq)
# speed: a burst of keys is not swallowed; labels follow the x2 steps
f, _ = run([(b"++", 0.2), (b"---", 0.1), (b"", 0.6)])
speeds = [x for _, x, _ in f]
if 4.0 not in speeds or 0.5 not in speeds:
    fail("speed labels %r" % sorted(set(speeds)))
# sessions table: reverse rows only hold, underline rows wait; the verdict names the top CS
f, txt = run([(b"", 3.0)], tail=0.2)
checked = full = 0
for frame in txt.split("\x1b[H\x1b[2J")[1:]:
    if "[sessions]" not in frame:
        continue
    for line in frame.partition("[sessions]")[2].splitlines():
        if re.match(r"\x1b\[7(;[\d;]*)?m", line) and ("waits" in line or "holds" not in line):
            fail("reverse session row is not hold-only: %r" % line)
        if re.match(r"\x1b\[4(;[\d;]*)?m", line) and "waits" not in line:
            fail("underline session row does not wait: %r" % line)
    if "[verdict]" in frame and "bottleneck" in frame and "[critical sections by waiting]" in frame:
        checked += 1
        # a CTVREC 5 record carries thread states and wait reasons
        if "[thread waits]" not in frame:
            fail("summary lacks the thread-wait block")
        full += "disk/cpu" in frame
if checked == 0:
    fail("no verdict frame seen")
if full == 0:
    fail("no verdict frame with the disk/cpu row (a writer-held CS)")
# colors on a terminal, none with --plain (reverse/underline marks stay)
if "\x1b[38;5;" not in txt:
    fail("no color on the terminal")
def run_plain():
    global TOOL
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(LANG="en_US.UTF-8", TERM="xterm", COLUMNS="140")
        os.execv(TOOL, ["cub_ctv", "--replay", REC, "--plain"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 140, 0, 0))
    buf = b""; t = time.time()
    while time.time() - t < 2.5:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: d = os.read(fd, 1 << 16)
            except OSError: break
            if not d: break
            buf += d
    os.write(fd, b"q"); time.sleep(0.3)
    try: os.kill(pid, 15)
    except ProcessLookupError: pass
    os.waitpid(pid, 0)
    return buf.decode("utf-8", "replace")
ptxt = run_plain()
if "\x1b[38;5;" in ptxt or ("\x1b[7m" not in ptxt and "\x1b[4m" not in ptxt):
    fail("--plain still colors, or lost the reverse/underline marks")
# pipe output carries no escape codes
import subprocess
out = subprocess.run([TOOL, "--replay", REC, "--replay-speed", "0"], stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, universal_newlines=True).stdout  # python 3.6
if "\x1b" in out:
    fail("escape code in pipe output")
# [l] toggles the language of the redrawn frame (footer REPLAY <-> 재생)
f, txt = run([(b" ", 1.3), (b"l", 0.4), (b"l", 0.4)])
if "재생 " not in txt or txt.count("REPLAY ") < 2:
    fail("[l] did not toggle the language (ko=%s)" % ("재생 " in txt))
# quit restores the terminal
f, txt = run([(b"q", 0.8)], tail=0.5)
if "\x1b[?1049l" not in txt:
    fail("alternate screen not restored on q")
# a damaged first frame is reported at its own line, as the scroll reader does
# (indexing for the interactive replay must not move the line count)
import tempfile
KBAD = """CTVREC 5
H 4242 1 100 0.5 512 0 160 2 152 demodb /opt/cubrid/lib/libcubrid.so.11.5 ~
F 1793193213 2 50 0.5 1 1 1 4 0 0
B 0
C 0 0 0 0 -1 0 0 1 120 0 0 3.5 1 0 0 -1 0 0 100 0 WFG 3 -1
C 1 0 0 0 -1 0 0 1 120 0 0 3.5 1 0 0 -1 0 0 100 0 LOG 3 -1
K 1
W 1 0 3 4300 999 -1
E
F 1793193214 2 50 0.5 1 1 1 4 0 0
B 0
C 0 0 0 0 -1 0 0 1 120 0 0 3.5 1 0 0 -1 0 0 100 0 WFG 3 -1
C 1 0 0 0 -1 0 0 1 120 0 0 3.5 1 0 0 -1 0 0 100 0 LOG 3 -1
K 0
E
"""
with tempfile.NamedTemporaryFile("w", suffix=".rec", delete=False) as kf:
    kf.write(KBAD)
try:
    f, txt = run([], tail=0.8, rec=kf.name)
finally:
    os.unlink(kf.name)
if "malformed record file at %s:8" % kf.name not in txt:
    fail("interactive replay reported the damaged line elsewhere: %r" % txt[-200:])
print("t8 PASS: pause / step / speed burst / quit / summary screen / damaged-line number (%d frames)" % checked)
